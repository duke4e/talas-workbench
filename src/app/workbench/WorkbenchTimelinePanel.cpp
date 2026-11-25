#include "app/workbench/WorkbenchTimelinePanel.h"

#include "app/workbench/WorkbenchTheme.h"
#include "app/workbench/WorkbenchTimelineInfoStrip.h"
#include "app/workbench/WorkbenchUiHelpers.h"
#include "talas/app/AutomationLaneEditing.h"
#include "talas/app/LiveRecordingModel.h"
#include "talas/app/SoundBrowserModel.h"
#include "talas/app/SoundFileWindowModel.h"
#include "talas/app/TimelineInfo.h"
#include "talas/core/FromChars.h"
#include "talas/project/ProjectGlobalMap.h"
#include "talas/app/TimelineGrid.h"
#include "talas/app/TimelineMarks.h"
#include "talas/app/TimelineTrackInput.h"
#include "talas/runtime/PreparedScore.h"
#include "talas/app/TimelineScoreProjection.h"
#include "talas/project/TimelineInteraction.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

namespace talas::app::workbench
{
namespace
{
[[nodiscard]] double frameToScrollValue(std::uint64_t frame) noexcept
{
    const auto roundedLimit = static_cast<double>(
        std::numeric_limits<std::uint64_t>::max());
    const auto safeLimit = std::nextafter(roundedLimit, 0.0);
    return static_cast<long double>(frame) >= static_cast<long double>(safeLimit)
        ? safeLimit
        : static_cast<double>(frame);
}

[[nodiscard]] std::uint64_t scrollValueToFrame(double value) noexcept
{
    if (!std::isfinite(value) || value <= 0.0)
        return 0;
    const auto roundedLimit = static_cast<double>(
        std::numeric_limits<std::uint64_t>::max());
    const auto safeLimit = std::nextafter(roundedLimit, 0.0);
    return static_cast<std::uint64_t>(std::floor(std::min(value, safeLimit)));
}

[[nodiscard]] std::optional<talas::project::ScoreTime> divideScoreTime(
    const talas::project::ScoreTime& value,
    std::uint32_t divisor) noexcept
{
    if (divisor == 0U || !talas::project::isCanonicalScoreTime(value))
        return std::nullopt;
    if (value.domain == talas::project::ScoreTimeDomain::Seconds)
    {
        const auto seconds = value.seconds / static_cast<double>(divisor);
        if (!std::isfinite(seconds) || seconds <= 0.0)
            return std::nullopt;
        return talas::project::ScoreTime{value.domain, seconds, 0};
    }
    const auto quotient = value.tick / divisor;
    const auto remainder = value.tick % divisor;
    const auto roundUp = remainder >= divisor / 2U + divisor % 2U;
    if (quotient == std::numeric_limits<std::uint64_t>::max() && roundUp)
        return std::nullopt;
    const auto ticks = quotient + static_cast<std::uint64_t>(roundUp);
    if (ticks == 0U)
        return std::nullopt;
    return talas::project::ScoreTime{value.domain, 0.0, ticks};
}

[[nodiscard]] juce::String unsignedText(std::uint64_t value)
{
    std::array<char, 32> buffer{};
    const auto [end, error] = std::to_chars(
        buffer.data(), buffer.data() + buffer.size(), value);
    return error == std::errc{}
        ? juce::String::fromUTF8(
              buffer.data(),
              static_cast<int>(end - buffer.data()))
        : juce::String{"?"};
}

[[nodiscard]] juce::String smptePositionText(
    const talas::runtime::RealtimeMusicalTransportSnapshot& snapshot)
{
    const auto& follower = snapshot.mtc;
    if (!snapshot.publicationValid
        || !snapshot.mtcAuthority
        || snapshot.mtcAuthorityGeneration == 0
        || snapshot.mtcStageId == 0
        || snapshot.mtcStageId != snapshot.stageId
        || follower.identity.authorityGeneration
            != snapshot.mtcAuthorityGeneration
        || follower.identity.stageId != snapshot.mtcStageId
        || !follower.hasTimeCode)
    {
        return {};
    }
    const auto& timeCode = follower.timeCode;
    auto rate = juce::String{"24 fps"};
    switch (timeCode.rate)
    {
    case talas::runtime::SmpteRate::Fps24: break;
    case talas::runtime::SmpteRate::Fps25: rate = "25 fps"; break;
    case talas::runtime::SmpteRate::Fps2997Drop: rate = "29.97 DF"; break;
    case talas::runtime::SmpteRate::Fps30: rate = "30 fps"; break;
    }
    return "SMPTE "
        + juce::String::formatted(
            "%02u:%02u:%02u:%02u",
            static_cast<unsigned>(timeCode.hour),
            static_cast<unsigned>(timeCode.minute),
            static_cast<unsigned>(timeCode.second),
            static_cast<unsigned>(timeCode.frame))
        + juceString(" · ") + rate;
}

[[nodiscard]] TimelineSmpteRate scoreSmpteRate(
    talas::runtime::SmpteRate rate) noexcept
{
    switch (rate)
    {
    case talas::runtime::SmpteRate::Fps24: return TimelineSmpteRate::Fps24;
    case talas::runtime::SmpteRate::Fps25: return TimelineSmpteRate::Fps25;
    case talas::runtime::SmpteRate::Fps2997Drop:
        return TimelineSmpteRate::Fps2997Drop;
    case talas::runtime::SmpteRate::Fps30: return TimelineSmpteRate::Fps30;
    }
    return TimelineSmpteRate::Fps24;
}

[[nodiscard]] TimelineScoreRuntimeState scoreRuntimeState(
    const talas::runtime::RealtimeMusicalTransportSnapshot& snapshot,
    const juce::String& externalDetail,
    const talas::project::ProjectDocument& document,
    const TimelineOutputMeterSnapshot& outputMeters)
{
    const auto sampleRate = std::isfinite(snapshot.transport.sampleRate)
            && snapshot.transport.sampleRate >= 1.0
            && snapshot.transport.sampleRate
                <= static_cast<double>(std::numeric_limits<std::uint32_t>::max())
        ? static_cast<std::uint32_t>(std::llround(snapshot.transport.sampleRate))
        : 48000U;
    auto sync = TimelineExternalSyncKind::Internal;
    auto locked = true;
    // Without time code coming in, the Timeline's
    // own frames a second, else the project's.
    const auto frames = talas::app::timelineFramesPerSecond(document);
    auto rate = frames == 25 ? TimelineSmpteRate::Fps25 : frames == 30 ? TimelineSmpteRate::Fps30 : TimelineSmpteRate::Fps24;
    if (snapshot.mtcAuthority)
    {
        sync = TimelineExternalSyncKind::MidiTimeCode;
        locked = snapshot.publicationValid
            && (snapshot.mtc.state == talas::runtime::MtcTransportLockState::Locked
                || snapshot.mtc.state == talas::runtime::MtcTransportLockState::Located);
        if (snapshot.mtc.hasTimeCode)
            rate = scoreSmpteRate(snapshot.mtc.timeCode.rate);
    }
    else if (snapshot.externalAuthority)
    {
        sync = TimelineExternalSyncKind::MidiClock;
        locked = snapshot.publicationValid
            && snapshot.externalLockState == talas::runtime::MidiClockLockState::Locked;
    }
    TimelineScoreRuntimeState result;
    result.stageId = snapshot.stageId;
    result.playheadFrame = snapshot.transport.playheadFrame;
    result.sampleRate = sampleRate;
    result.playing = snapshot.transport.playing;
    result.publicationValid = snapshot.publicationValid;
    result.syncKind = sync;
    result.externalLocked = locked;
    result.smpteRate = rate;
    result.externalAccessibilityText = externalDetail.toStdString();
    const auto master = std::ranges::find_if(
        document.timeline.score.tracks,
        [](const auto& track) {
            return track.kind == talas::project::ScoreTrackKind::Master;
        });
    if (master != document.timeline.score.tracks.end()
        && outputMeters.stageId != 0
        && outputMeters.stageId == snapshot.stageId)
    {
        auto peak = 0.0F;
        for (const auto value : outputMeters.peaks)
        {
            if (std::isfinite(value) && value >= 0.0F)
                peak = std::max(peak, value);
        }
        result.meters[0] = {master->trackId, static_cast<double>(peak)};
        result.meterCount = 1;
    }
    return result;
}

[[nodiscard]] std::optional<talas::project::ScoreTime> addScoreTime(
    const talas::project::ScoreTime& left,
    const talas::project::ScoreTime& right) noexcept
{
    if (left.domain != right.domain)
        return std::nullopt;
    if (left.domain == talas::project::ScoreTimeDomain::Seconds)
    {
        const auto value = left.seconds + right.seconds;
        return std::isfinite(value)
            ? std::optional<talas::project::ScoreTime>{{left.domain, value, 0}}
            : std::nullopt;
    }
    if (right.tick > std::numeric_limits<std::uint64_t>::max() - left.tick)
        return std::nullopt;
    return talas::project::ScoreTime{left.domain, 0.0, left.tick + right.tick};
}

[[nodiscard]] std::optional<talas::project::ScoreTime> subtractScoreTime(
    const talas::project::ScoreTime& left,
    const talas::project::ScoreTime& right) noexcept
{
    if (left.domain != right.domain)
        return std::nullopt;
    if (left.domain == talas::project::ScoreTimeDomain::Seconds)
    {
        const auto value = left.seconds - right.seconds;
        return std::isfinite(value) && value >= 0.0
            ? std::optional<talas::project::ScoreTime>{{left.domain, value, 0}}
            : std::nullopt;
    }
    if (right.tick > left.tick)
        return std::nullopt;
    return talas::project::ScoreTime{left.domain, 0.0, left.tick - right.tick};
}

[[nodiscard]] std::optional<talas::project::ScoreTime> multiplyScoreTime(
    const talas::project::ScoreTime& value,
    std::uint32_t multiplier) noexcept
{
    if (multiplier == 0U || !talas::project::isCanonicalScoreTime(value))
        return std::nullopt;
    if (value.domain == talas::project::ScoreTimeDomain::Seconds)
    {
        const auto seconds = value.seconds * static_cast<double>(multiplier);
        return std::isfinite(seconds)
            ? std::optional<talas::project::ScoreTime>{{value.domain, seconds, 0}}
            : std::nullopt;
    }
    if (value.tick > std::numeric_limits<std::uint64_t>::max() / multiplier)
        return std::nullopt;
    return talas::project::ScoreTime{
        value.domain, 0.0, value.tick * multiplier};
}

[[nodiscard]] bool positiveScoreTime(
    const talas::project::ScoreTime& value) noexcept
{
    return value.domain == talas::project::ScoreTimeDomain::Seconds
        ? value.seconds > 0.0
        : value.tick > 0U;
}

[[nodiscard]] talas::project::ScoreTime minimumScoreTime(
    const talas::project::ScoreTime& left,
    const talas::project::ScoreTime& right) noexcept
{
    if (left.domain != right.domain)
        return left;
    if (left.domain == talas::project::ScoreTimeDomain::Seconds)
        return left.seconds <= right.seconds ? left : right;
    return left.tick <= right.tick ? left : right;
}

[[nodiscard]] bool clampClipFadesToTotalDuration(
    talas::project::ScoreClip& clip,
    const talas::project::ScoreTime& totalDuration) noexcept
{
    clip.fadeIn = minimumScoreTime(clip.fadeIn, totalDuration);
    const auto remaining = subtractScoreTime(totalDuration, clip.fadeIn);
    if (!remaining.has_value())
        return false;
    clip.fadeOut = minimumScoreTime(clip.fadeOut, *remaining);
    return true;
}

class ScoreViewportComponent final : public juce::Component,
                                     public juce::DragAndDropTarget,
                                     private juce::ScrollBar::Listener
{
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
            ScoreViewportComponent& owner,
            TimelineScoreSelection selection,
            std::string focusIdentity,
            juce::String title)
            : owner_(owner)
            , selection_(std::move(selection))
            , focusIdentity_(std::move(focusIdentity))
            , title_(std::move(title))
        {
            setComponentID(juceString(focusIdentity_));
            setAccessible(true);
            setWantsKeyboardFocus(true);
            setInterceptsMouseClicks(false, false);
            describeControl(*this, title_, title_);
        }

        void update(
            TimelineScoreSelection selection,
            juce::String title,
            juce::Rectangle<int> bounds)
        {
            selection_ = std::move(selection);
            if (title_ != title)
            {
                title_ = std::move(title);
                describeControl(*this, title_, title_);
            }
            if (getBounds() != bounds)
                setBounds(bounds);
        }

        [[nodiscard]] std::string_view focusIdentity() const noexcept
        {
            return focusIdentity_;
        }

        [[nodiscard]] bool selected() const noexcept
        {
            return owner_.selection_ == selection_;
        }

        bool keyPressed(const juce::KeyPress& key) override
        {
            owner_.selectAccessibleEntity(selection_, focusIdentity_);
            return owner_.handleAccessibleKey(key, focusIdentity_);
        }

        bool keyStateChanged(bool isKeyDown) override
        {
            return owner_.keyStateChanged(isKeyDown);
        }

        void focusGained(juce::Component::FocusChangeType) override
        {
            owner_.selectAccessibleEntity(selection_, focusIdentity_);
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
                            safeThis->owner_.selectAccessibleEntity(
                                safeThis->selection_,
                                safeThis->focusIdentity_);
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
                    return entity_.selected() ? state.withSelected() : state;
                }

            private:
                AccessibilityEntity& entity_;
            };
            return std::make_unique<Handler>(*this, std::move(actions));
        }

    private:
        ScoreViewportComponent& owner_;
        TimelineScoreSelection selection_;
        std::string focusIdentity_;
        juce::String title_;
    };

public:
    using DocumentChangeCallback = std::function<void()>;
    using StatusCallback = std::function<void(const juce::String&)>;
    using AcceptedStageCurrentCallback = std::function<bool()>;
    using ClipArtifactStateCallback =
        TimelinePanel::ClipArtifactStateCallback;
    using ClipArtifactActionCallback =
        TimelinePanel::ClipArtifactActionCallback;

    ScoreViewportComponent(
        talas::runtime::ProjectRuntimeBridge& bridge,
        DocumentChangeCallback onDocumentChange,
        StatusCallback onStatus,
        AcceptedStageCurrentCallback acceptedStageCurrent,
        ClipArtifactStateCallback clipArtifactState,
        ClipArtifactActionCallback clipArtifactAction)
        : bridge_(bridge)
        , onDocumentChange_(std::move(onDocumentChange))
        , onStatus_(std::move(onStatus))
        , acceptedStageCurrent_(std::move(acceptedStageCurrent))
        , clipArtifactState_(std::move(clipArtifactState))
        , clipArtifactAction_(std::move(clipArtifactAction))
    {
        setComponentID("timeline.score");
        setWantsKeyboardFocus(true);
        setFocusContainerType(juce::Component::FocusContainerType::focusContainer);
        accessibilityContainer_.setComponentID(
            "timeline.score.accessibility");
        accessibilityContainer_.setAccessible(true);
        accessibilityContainer_.setInterceptsMouseClicks(false, false);
        addAndMakeVisible(accessibilityContainer_);
        describeControl(
            *this,
            "Timeline Score",
            "Virtualized tracks, Sound clips, automation, markers, routing, and transport position. Pointer motion publishes an exact grid candidate; hold Command while clicking or dragging, or Shift with K or P, to accept that candidate. Otherwise edits preserve exact time. Press C for exact actions and Sound choices, E to start or stop recording the selected automation lane, V to preview simplification, and Return to approve that exact preview. Use Alt plus Left or Right to nudge, Escape to cancel, arrow keys to move focus, and plus or minus to zoom.");
        setTitle("Timeline Score loading.");

        rulerMode_.addItem("Seconds", 1);
        rulerMode_.addItem(juceString("Bars · Beats · Ticks"), 2);
        rulerMode_.addItem("SMPTE", 3);
        rulerMode_.addItem(juceString("Minutes · Seconds"), 4);
        rulerMode_.setSelectedId(2, juce::dontSendNotification);
        rulerMode_.setComponentID("timeline.score.ruler");
        describeControl(
            rulerMode_,
            "Timeline ruler",
            "Choose seconds, bars beats ticks, or SMPTE presentation. This does not rewrite Score times.");
        rulerMode_.onChange = [this] {
            rebuildViewport();
            repaint();
        };
        addAndMakeVisible(rulerMode_);
        // The Timeline's own time code frames a
        // second, overriding the project's.
        framesMenu_.addItem("Project fps", 1);
        for (const auto frames : {24, 25, 30})
            framesMenu_.addItem(juce::String(frames) + " fps", frames);
        describeControl(framesMenu_, "Time code frames a second",
            "The Timeline's own time code frames a second, or the project's");
        framesMenu_.onChange = [this] {
            const auto id = framesMenu_.getSelectedId();
            const auto edited = talas::app::timelineWithFramesPerSecond(bridge_.editor().document(), id == 1 ? 0U : static_cast<unsigned>(id));
            if (edited.has_value() && edited->timeline.timeCodeFramesPerSecond != bridge_.editor().document().timeline.timeCodeFramesPerSecond)
                (void) commitInfoEdit(bridge_.editor().replaceDocument(*edited), "The Timeline's time code frames a second changed");
        };
        addAndMakeVisible(framesMenu_);

        // The Grid menu, its mode's name on its
        // button, and the quantization, a fraction of a frame, a beat or a
        // second as the ruler's units are.
        for (const auto mode : {talas::app::TimelineGridMode::Free, talas::app::TimelineGridMode::Quantized,
                 talas::app::TimelineGridMode::Magnet, talas::app::TimelineGridMode::EnterTime})
            gridMode_.addItem(juceString(std::string{talas::app::timelineGridModeName(mode)}), static_cast<int>(mode) + 1);
        gridMode_.setSelectedId(1, juce::dontSendNotification);
        gridMode_.setComponentID("timeline.score.grid");
        describeControl(gridMode_, "Grid",
            "Free grid places at any time; Quantized grid on the quantization only; Magnet grid starts a Sound dropped on a track where the one before it ends; Enter beat or time leaves Sounds where they are for times typed in the strip.");
        gridMode_.onChange = [this] { (void) setGridMode(static_cast<talas::app::TimelineGridMode>(gridMode_.getSelectedId() - 1)); };
        addAndMakeVisible(gridMode_);
        refreshQuantizationMenu();
        quantization_.setComponentID("timeline.score.quantization");
        describeControl(quantization_, "Quantization",
            "The smallest step for placing and nudging: a fraction of a frame, a beat or a second, as the ruler shows time. Choose Enter... to type your own.");
        quantization_.onChange = [this] {
            const auto id = quantization_.getSelectedId();
            const auto choices = talas::app::timelineQuantizationChoices();
            if (id >= 1 && static_cast<std::size_t>(id) <= choices.size())
                (void) setQuantization(choices[static_cast<std::size_t>(id - 1)]);
            else if (id == quantizationEnterId)
                askForQuantization();
        };
        addAndMakeVisible(quantization_);

        // The channel whose program changes jump the
        // cursor to markers, beside the ruler's units.
        programChannel_.addItem("No PC", 1);
        for (int channel = 1; channel <= 16; ++channel)
            programChannel_.addItem(juce::String("PC Ch ") + juce::String(channel), channel + 1);
        programChannel_.setSelectedId(1, juce::dontSendNotification);
        programChannel_.setComponentID("timeline.score.program-channel");
        describeControl(
            programChannel_,
            "Timeline program change channel",
            "The MIDI channel whose program changes jump the time cursor to the markers, bypassing the virtual control surface's presets.");
        programChannel_.onChange = [this] {
            (void) setMarkerProgramChannel(static_cast<std::uint8_t>(std::max(0, programChannel_.getSelectedId() - 1)));
        };
        addAndMakeVisible(programChannel_);

        infoStrip_.onField = [this](TimelineInfoField field, const std::string& text) {
            return applyInfoField(field, text);
        };
        infoStrip_.onMarkerProgram = [this](std::uint8_t program) { return setSelectedMarkerProgram(program); };
        infoStrip_.onMarkerButton = [this] { showMarkerDialog(); };
        infoStrip_.onAnchorButton = [this] { return anchorSelectedClipAtCursor(); };
        // The colour menu colours the selected Sounds.
        infoStrip_.onColour = [this](std::optional<std::uint32_t> rgb) {
            const auto ids = selectedClipIds();
            const auto coloured = talas::app::timelineWithClipColour(bridge_.editor().document(), ids, rgb);
            if (!coloured.has_value())
                return false;
            talas::project::ProjectScoreEditBatch batch;
            for (const auto& clip : coloured->timeline.score.clips)
            {
                if (std::ranges::find(ids, clip.clipId) != ids.end())
                    batch.clips.push_back(clip);
            }
            return commitInfoEdit(bridge_.editor().applyScoreEdits(std::move(batch)), "Coloured the selected Sounds");
        };
        infoStrip_.onSpace = [this] {
            if (spaceKey_)
                spaceKey_();
        };
        infoStrip_.setComponentID("timeline.score.info");
        addAndMakeVisible(infoStrip_);

        zoom_.setSliderStyle(juce::Slider::LinearHorizontal);
        zoom_.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 46, 20);
        zoom_.setRange(0.25, 16.0, 0.25);
        zoom_.setValue(1.0, juce::dontSendNotification);
        zoom_.setNumDecimalPlacesToDisplay(2);
        zoom_.setTextValueSuffix(juceString("×"));
        zoom_.setComponentID("timeline.score.zoom");
        describeControl(
            zoom_,
            "Timeline zoom",
            "Change horizontal Score magnification without editing the project.");
        zoom_.onValueChange = [this] {
            updateScrollRanges();
            rebuildViewport();
            repaint();
        };
        addAndMakeVisible(zoom_);
        // The time cursor's source of external
        // control, Free running or Time code, and the time code offset.
        timeSource_.setComponentID("timeline.score.time-source");
        describeControl(timeSource_, "Time cursor source", "Free running, or the time cursor follows time code.");
        timeSource_.onClick = [this] { showTimeSourceMenu(); };
        addAndMakeVisible(timeSource_);
        cacheMenu_.setComponentID("timeline.score.cache");
        describeControl(cacheMenu_, "Cache menu",
            "The selected Sound played live, cached on disk or cached again: green live, red while caching, blue cached.");
        cacheMenu_.onClick = [this] { showCacheMenu(); };
        addAndMakeVisible(cacheMenu_);
        // The Audio menu, the Timeline's outputs
        // and the format its caches are recorded in.
        describeControl(audioMenu_, "Audio menu",
            "The number of the Timeline's outputs, the Master's channels, and the caches' file format.");
        audioMenu_.onClick = [this] {
            juce::PopupMenu outputs;
            const auto current = talas::app::timelineOutputs(bridge_.editor().document());
            for (const auto count : {1U, 2U, 4U, 6U, 8U})
                outputs.addItem(static_cast<int>(count), juce::String(static_cast<int>(count)) + (count == 1 ? " output" : " outputs"),
                    current != 0, count == current);
            juce::PopupMenu menu;
            menu.addSubMenu("Outputs", outputs, current != 0);
            juce::PopupMenu cacheFormats;
            const auto bits = bridge_.editor().document().timeline.cacheWordSize;
            for (const auto size : {24U, 16U})
                cacheFormats.addItem(static_cast<int>(cacheFormatItemBase + size),
                    juceString(talas::app::timelineCacheFormatName(size)), true, bits == size);
            menu.addSubMenu("Cache file format", cacheFormats);
            // A reader and its writer on separate
            // tracks, scheduled together (format 156).
            const auto sharing = bridge_.editor().document().timeline.sharingMemoryWritersAcrossTracks;
            menu.addItem(sharingAcrossTracksItem, juceString(talas::app::timelineSharingAcrossTracksName()), true, sharing);
            juce::Component::SafePointer<ScoreViewportComponent> safeThis(this);
            menu.showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(&audioMenu_), [safeThis, sharing](int chosen) {
                if (safeThis == nullptr || chosen <= 0)
                    return;
                if (chosen == sharingAcrossTracksItem)
                    (void) safeThis->setTimelineSharingAcrossTracks(!sharing);
                else if (chosen > cacheFormatItemBase)
                    (void) safeThis->setTimelineCacheWordSize(static_cast<unsigned>(chosen - cacheFormatItemBase));
                else
                    (void) safeThis->setTimelineOutputs(static_cast<std::uint32_t>(chosen));
            });
        };
        addAndMakeVisible(audioMenu_);
        // Zoom out, zoom in and zoom to fit.
        zoomOut_.setComponentID("timeline.score.zoom-out");
        describeControl(zoomOut_, "Zoom out", "Show more time in the window.");
        zoomOut_.onClick = [this] { zoom_.setValue(talas::app::timelineZoomedOut(zoom_.getValue())); };
        addAndMakeVisible(zoomOut_);
        zoomIn_.setComponentID("timeline.score.zoom-in");
        describeControl(zoomIn_, "Zoom in", "Show less time in the window.");
        zoomIn_.onClick = [this] { zoom_.setValue(talas::app::timelineZoomedIn(zoom_.getValue())); };
        addAndMakeVisible(zoomIn_);
        zoomFit_.setComponentID("timeline.score.zoom-fit");
        describeControl(zoomFit_, "Zoom to fit", "Fit the entire Timeline within the window without scrolling.");
        zoomFit_.onClick = [this] {
            zoom_.setValue(talas::app::timelineZoomToFit());
            horizontal_.setCurrentRangeStart(0.0);
        };
        addAndMakeVisible(zoomFit_);

        horizontal_.addListener(this);
        vertical_.addListener(this);
        horizontal_.setAutoHide(true);
        vertical_.setAutoHide(true);
        horizontal_.setComponentID("timeline.score.horizontal-scroll");
        vertical_.setComponentID("timeline.score.vertical-scroll");
        describeControl(
            horizontal_,
            "Timeline horizontal scroll",
            "Move the visible Score time range.");
        describeControl(
            vertical_,
            "Timeline vertical scroll",
            "Move through track and automation rows.");
        addAndMakeVisible(horizontal_);
        addAndMakeVisible(vertical_);
    }

    ~ScoreViewportComponent() override
    {
        // A recording in progress stops and finishes before the panel goes.
        recordingCancelled_->store(true);
        recordingPool_.reset();
        cancelOwnedGesture(false);
        horizontal_.removeListener(this);
        vertical_.removeListener(this);
    }

    // Marking a Sound, or every marked one cleared, and the mode the
    // Timeline plays in.
    bool markClip(const std::string& clipId, bool toggle)
    {
        if (toggle)
            marks_.toggle(clipId);
        else
            marks_.mark(clipId);
        repaint();
        onStatus_(marks_.marked(clipId) ? "Marked the Sound; Shift+Space plays the marked Sounds only"
                                        : "Unmarked the Sound");
        return true;
    }

    [[nodiscard]] talas::app::TimelineMarks& marks() noexcept
    {
        return marks_;
    }

    bool takeProgramChange(std::uint8_t channel, std::uint8_t program)
    {
        return jumpToMarkerForProgramChange(channel, program);
    }

    void rebuild(
        const talas::project::ProjectDocument& document,
        TimelineScoreRuntimeState runtime,
        bool preserveOwnedGesture = false)
    {
        refreshControllerRecordingState();
        if (!preserveOwnedGesture)
            revokeOwnedGestureForExternalRebuild();
        reconcileSelectedAutomationPoint(document);
        runtime_ = std::move(runtime);
        runtime_.laneValueZooms = laneValueZooms_;
        projection_ = std::make_unique<TimelineScoreProjection>(
            document,
            runtime_);
        timeSource_.setButtonText(juceString(talas::app::timelineTimeSourceLabel(document)));
        // The tracks' Angle and Radius for
        // their Panning status.
        auto panning = talas::runtime::prepareProjectScore(document,
            talas::runtime::preparedScoreBindingForDocument(document, 1, 1, activeSampleRate(), 1));
        panningSchedule_ = panning.ok() ? std::move(panning.schedule) : nullptr;
        updateScrollRanges();
        rebuildViewport();
        repaint();
    }

    void updateRuntime(TimelineScoreRuntimeState runtime)
    {
        refreshControllerRecordingState();
        const auto sampleRateChanged = runtime.sampleRate != runtime_.sampleRate;
        runtime_ = std::move(runtime);
        runtime_.laneValueZooms = laneValueZooms_;
        if (!projection_ || sampleRateChanged)
            return;
        projection_->updateRuntimeState(runtime_);
        const auto requestedExtent = static_cast<long double>(runtime_.playheadFrame)
            + static_cast<long double>(runtime_.sampleRate) * 2.0L;
        if (requestedExtent
            > static_cast<long double>(horizontal_.getMaximumRangeLimit()))
        {
            updateScrollRanges();
        }
        rebuildViewport();
        repaint(contentBounds());
    }

    void setExternalDetail(const juce::String& detail)
    {
        runtime_.externalAccessibilityText = detail.toStdString();
        if (projection_)
        {
            projection_->updateRuntimeState(runtime_);
            rebuildViewport();
            repaint();
        }
    }

    void paint(juce::Graphics& graphics) override
    {
        updateCacheMenuLook();
        graphics.fillAll(backgroundColour);
        auto body = contentBounds();
        graphics.setColour(panelBorderColour);
        graphics.drawRect(body, 1);

        if (!projection_ || viewport_.state != TimelineScoreSurfaceState::Content)
        {
            graphics.setColour(viewport_.state == TimelineScoreSurfaceState::Error
                    ? errorColour
                    : secondaryTextColour);
            graphics.setFont(WorkbenchType::bodyStrong());
            const auto text = viewport_.state == TimelineScoreSurfaceState::Error
                ? juceString(viewport_.diagnostic)
                : viewport_.state == TimelineScoreSurfaceState::Loading
                ? juceString("Loading Score…")
                : juce::String{"Score is empty"};
            graphics.drawFittedText(
                text,
                body.reduced(18),
                juce::Justification::centred,
                3);
            if (!viewport_.externalSyncText.empty())
            {
                graphics.setColour(secondaryTextColour);
                graphics.setFont(WorkbenchType::micro());
                graphics.drawFittedText(
                    juceString(viewport_.externalSyncText),
                    body.reduced(14).removeFromBottom(28),
                    juce::Justification::centred,
                    2);
            }
            return;
        }

        const auto headerWidth = scoreHeaderWidth();
        const auto layout = talas::app::timelineScoreLayout(body.getWidth(), body.getHeight(), headerWidth, scoreStripHeight());
        const auto area = [origin = body.getPosition()](const talas::app::TimelineScoreArea& part) {
            return juce::Rectangle<int>{origin.x + part.x, origin.y + part.y, part.width, part.height};
        };
        const auto markerStrip = area(layout.markers);
        const auto timeDisplay = area(layout.timeDisplay);
        auto rulerTrack = area(layout.ruler);
        auto ruler = body.removeFromTop(scoreRulerHeight());
        graphics.setGradientFill(juce::ColourGradient{panelColour.brighter(0.08F), 0.0F, static_cast<float>(ruler.getY()),
            panelColour.darker(0.1F), 0.0F, static_cast<float>(ruler.getBottom()), false});
        graphics.fillRect(ruler);
        graphics.setColour(workbenchTheme.well.withAlpha(0.7F));
        graphics.fillRect(markerStrip);
        graphics.setColour(panelBorderColour);
        graphics.drawHorizontalLine(markerStrip.getBottom(),
                                    static_cast<float>(ruler.getX()),
                                    static_cast<float>(ruler.getRight()));
        graphics.drawHorizontalLine(ruler.getBottom(),
                                    static_cast<float>(ruler.getX()),
                                    static_cast<float>(ruler.getRight()));
        graphics.drawVerticalLine(rulerTrack.getX(),
                                  static_cast<float>(ruler.getY()),
                                  static_cast<float>(getHeight()));
        // The cursor's time at the ruler's left.
        graphics.setColour(textColour);
        graphics.setFont(WorkbenchType::microStrong());
        graphics.drawText(runtime_.sampleRate > 0
                ? juce::String(static_cast<double>(runtime_.playheadFrame) / static_cast<double>(runtime_.sampleRate), 3) + " s"
                : juce::String{},
            timeDisplay.reduced(7, 0), juce::Justification::centredLeft);
        graphics.setColour(secondaryTextColour);
        ruler = ruler.withHeight(markerStrip.getHeight());
        const auto authorityText = runtime_.syncKind
                == TimelineExternalSyncKind::MidiTimeCode
            ? (runtime_.externalLocked
                   ? juceString("MTC · LOCK")
                   : juceString("MTC · WAIT"))
            : runtime_.syncKind == TimelineExternalSyncKind::MidiClock
            ? (runtime_.externalLocked
                   ? juceString("CLOCK · LOCK")
                   : juceString("CLOCK · WAIT"))
            : "INTERNAL";
        graphics.drawText(authorityText, ruler.removeFromLeft(headerWidth).reduced(7, 0),
                          juce::Justification::centredLeft);

        for (const auto& tick : viewport_.rulerTicks)
        {
            const auto x = rulerTrack.getX() + static_cast<float>(tick.x);
            graphics.setColour(tick.major
                    ? panelBorderColour.brighter(0.35F)
                    : panelBorderColour);
            graphics.drawVerticalLine(
                static_cast<int>(std::lround(x)),
                static_cast<float>(rulerTrack.getY() + (tick.major ? 2 : 10)),
                static_cast<float>(body.getBottom()));
            if (tick.major)
            {
                graphics.setColour(secondaryTextColour);
                graphics.setFont(WorkbenchType::micro());
                graphics.drawText(
                    juceString(tick.label),
                    static_cast<int>(std::lround(x)) + 4,
                    rulerTrack.getY(),
                    96,
                    rulerTrack.getHeight(),
                    juce::Justification::centredLeft,
                    false);
            }
        }

        auto rowsArea = body;
        const auto rowHeight = scoreRowHeight();
        for (std::size_t visibleIndex = 0;
             visibleIndex < viewport_.rows.size();
             ++visibleIndex)
        {
            const auto& row = viewport_.rows[visibleIndex];
            const auto y = rowsArea.getY()
                + static_cast<int>(visibleIndex) * rowHeight;
            auto rowBounds = juce::Rectangle<int>{
                rowsArea.getX(), y, rowsArea.getWidth(), rowHeight};
            auto header = rowBounds.removeFromLeft(headerWidth);
            graphics.setColour(row.selected
                    ? workbenchTheme.selectedRow
                    : (visibleIndex % 2U) == 0U
                    ? panelColour.brighter(0.04F)
                    : panelColour.darker(0.05F));
            graphics.fillRect(header);
            graphics.setColour((visibleIndex % 2U) == 0U
                    ? workbenchTheme.well
                    : workbenchTheme.well.interpolatedWith(panelColour, 0.3F));
            graphics.fillRect(rowBounds);
            // A control's editor is darker where
            // there is a Sound it could affect.
            if (row.kind == TimelineScoreRowKind::Automation)
            {
                const auto affected = talas::app::automationLaneAffectedClips(bridge_.editor().document().timeline.score, row.stableId);
                graphics.setColour(backgroundColour.darker(0.25F));
                for (const auto& item : viewport_.items)
                {
                    if (item.kind != TimelineScoreItemKind::Clip || std::ranges::find(affected, item.stableId) == affected.end())
                        continue;
                    const auto left = std::max(static_cast<float>(rowBounds.getX()), static_cast<float>(rowBounds.getX() + item.x));
                    const auto right = std::min(static_cast<float>(rowBounds.getRight()),
                        static_cast<float>(rowBounds.getX() + item.x + std::max(2.0, item.width)));
                    if (right > left)
                        graphics.fillRect(juce::Rectangle<float>(left, static_cast<float>(rowBounds.getY()), right - left,
                            static_cast<float>(rowBounds.getHeight())));
                }
            }
            graphics.setColour(panelBorderColour);
            graphics.drawHorizontalLine(
                rowBounds.getBottom(),
                static_cast<float>(header.getX()),
                static_cast<float>(rowBounds.getRight()));
            const auto kindColour = row.kind == TimelineScoreRowKind::Automation
                ? controlColour
                : row.kind == TimelineScoreRowKind::Master
                ? outputColour
                : audioColour;
            {
                const auto stripe = header.removeFromLeft(3).toFloat().reduced(0.0F, 3.0F);
                graphics.setGradientFill(juce::ColourGradient{kindColour.brighter(0.2F), stripe.getX(), stripe.getY(),
                    kindColour.darker(0.2F), stripe.getX(), stripe.getBottom(), false});
                graphics.fillRoundedRectangle(stripe, 1.5F);
            }
            header.reduce(6, 3);
            auto nameArea = header.removeFromTop(rowHeight / 2);
            if (row.automationState != talas::project::ScoreAutomationState::Automated)
            {
                // A Live lane's green dot, a Record-enabled lane's red one.
                graphics.setColour(row.automationState == talas::project::ScoreAutomationState::Live
                        ? juce::Colours::limegreen
                        : errorColour);
                graphics.fillEllipse(nameArea.removeFromLeft(10).withSizeKeepingCentre(7, 7).toFloat());
                nameArea.removeFromLeft(3);
            }
            // A Sound's parameter names turn red
            // while it is cached.
            const auto& lanes = bridge_.editor().document().timeline.score.automation;
            const auto lane = row.kind == TimelineScoreRowKind::Automation
                ? std::ranges::find(lanes, row.stableId, &talas::project::ScoreAutomationLane::laneId)
                : lanes.end();
            const auto cachingColour = lane == lanes.end()
                ? std::nullopt
                : talas::app::timelineLaneNameColour(*lane, cacheLookOf(lane->ownerId));
            graphics.setColour(cachingColour.has_value() ? juce::Colour{*cachingColour}
                    : row.muted                          ? secondaryTextColour
                                                         : textColour);
            graphics.setFont(WorkbenchType::labelStrong());
            graphics.drawText(
                juceString(row.displayName),
                nameArea,
                juce::Justification::centredLeft,
                true);
            graphics.setColour(secondaryTextColour);
            graphics.setFont(WorkbenchType::micro());
            // The input's name in its submix's
            // colour, and the Submix status, a square of the colour of the
            // submix the track sends to.
            // Within the square brackets, the input source or the
            // double-dash of none.
            if (row.kind == TimelineScoreRowKind::Track)
            {
                graphics.setColour(row.inputTrackId.empty() ? secondaryTextColour : submixColour(row.inputTrackId));
                graphics.drawText("[" + juceString(talas::app::timelineTrackInputLabel(
                                            bridge_.editor().document().timeline.score, row.stableId)) + "]",
                    header.removeFromLeft(std::min(header.getWidth() / 2, 64)), juce::Justification::centredLeft, true);
                graphics.setColour(secondaryTextColour);
            }
            if (row.kind == TimelineScoreRowKind::Track)
            {
                // The input's status indicators, the microphone and
                // the MIDI connector, green while the Sound under the time
                // cursor expects audio or MIDI input; the MIDI one names
                // the track's channel.
                const auto input = trackInputStatus(firstVisibleRow_ + visibleIndex);
                const auto track = std::ranges::find(bridge_.editor().document().timeline.score.tracks, row.stableId,
                    &talas::project::ScoreTrack::trackId);
                const auto midiChannel = track == bridge_.editor().document().timeline.score.tracks.end() ? 0 : track->midiChannel;
                auto midiArea = header.removeFromRight(14);
                // Red while the Sound's parameters are recorded.
                graphics.setColour(input.recording ? errorColour : input.midi ? juce::Colours::limegreen : panelBorderColour);
                graphics.drawEllipse(midiArea.withSizeKeepingCentre(9, 9).toFloat(), 1.2F);
                if (midiChannel != 0)
                {
                    graphics.setFont(WorkbenchType::micro());
                    graphics.drawText(juce::String(static_cast<int>(midiChannel)), midiArea, juce::Justification::centred, false);
                }
                graphics.setColour(input.audio ? juce::Colours::limegreen : panelBorderColour);
                graphics.fillRoundedRectangle(header.removeFromRight(9).withSizeKeepingCentre(4, 8).toFloat(), 2.0F);
                // The Output channel status of a hard-wired
                // track, a green square for each channel it goes to.
                const auto lit = talas::app::timelineOutputStatus(bridge_.editor().document().timeline.score, row.stableId);
                // Otherwise the Panning status, the listener at
                // the centre of the room and the apparent source of the
                // sound under the time cursor in yellow.
                if (lit.empty() && panningSchedule_ != nullptr)
                {
                    const auto tracks = panningSchedule_->tracks();
                    const auto found = std::ranges::find(tracks, row.stableId, &talas::runtime::PreparedScoreTrack::trackId);
                    if (found != tracks.end())
                    {
                        const auto source = talas::runtime::scoreTrackSourcePosition(*panningSchedule_,
                            static_cast<std::uint32_t>(found - tracks.begin()), runtime_.playheadFrame);
                        const auto room = header.removeFromRight(16).withSizeKeepingCentre(12, 12).toFloat();
                        graphics.setColour(panelBorderColour);
                        graphics.drawEllipse(room, 1.0F);
                        graphics.setColour(juce::Colours::black);
                        graphics.fillEllipse(room.withSizeKeepingCentre(2.5F, 2.5F));
                        const auto scale = room.getWidth() * 0.5F;
                        const auto distance = std::hypot(source.x, source.y);
                        const auto shown = distance > 1.2 ? 1.2 / distance : 1.0;
                        graphics.setColour(juce::Colours::yellow);
                        graphics.fillEllipse(juce::Rectangle<float>(3.5F, 3.5F).withCentre({
                            room.getCentreX() + static_cast<float>(source.x * shown) * scale,
                            room.getCentreY() - static_cast<float>(source.y * shown) * scale}));
                    }
                }
                for (const auto on : lit)
                {
                    graphics.setColour(on ? juce::Colours::limegreen : panelBorderColour);
                    graphics.fillRect(header.removeFromRight(5).withSizeKeepingCentre(4, 4));
                }
                const auto status = talas::app::timelineSubmixStatus(bridge_.editor().document().timeline.score, row.stableId);
                for (const auto colour : status)
                {
                    graphics.setColour(submixColourAt(colour));
                    graphics.fillRect(header.removeFromLeft(6).withSizeKeepingCentre(5, 5));
                }
                if (!status.empty())
                    header.removeFromLeft(3);
                graphics.setColour(secondaryTextColour);
            }
            auto details = row.outputTrackId.empty()
                ? juce::String{"automation"}
                : juceString(row.outputTrackId);
            if (row.muted)
                details += juceString(" · M");
            if (row.solo)
                details += juceString(" · S");
            graphics.drawText(details, header,
                              juce::Justification::centredLeft, true);
            if (row.meterAvailable)
            {
                const auto meterWidth = static_cast<int>(std::lround(
                    std::clamp(row.meterPeak, 0.0, 1.0) * 34.0));
                graphics.setColour(panelBorderColour);
                graphics.fillRect(
                    header.getRight() - 36,
                    header.getBottom() - 3,
                    34,
                    2);
                graphics.setColour(audioColour);
                graphics.fillRect(
                    header.getRight() - 36,
                    header.getBottom() - 3,
                    meterWidth,
                    2);
            }
        }

        auto timelineArea = rowsArea;
        timelineArea.removeFromLeft(headerWidth);
        for (const auto& item : viewport_.items)
        {
            const auto localRow = item.row >= firstVisibleRow_
                ? item.row - firstVisibleRow_
                : 0;
            auto itemBounds = juce::Rectangle<float>{
                static_cast<float>(timelineArea.getX() + item.x),
                static_cast<float>(timelineArea.getY()
                    + static_cast<int>(localRow) * rowHeight + 4),
                static_cast<float>(std::max(2.0, item.width)),
                static_cast<float>(rowHeight - 8)};
            if (item.kind == TimelineScoreItemKind::Marker)
            {
                // A marker's flag and name in the strip above
                // the ruler, its line down through the tracks.
                graphics.setColour(item.selected ? selectionColour : eventColour);
                graphics.drawVerticalLine(
                    static_cast<int>(std::lround(itemBounds.getX())),
                    static_cast<float>(markerStrip.getY()),
                    static_cast<float>(timelineArea.getBottom()));
                graphics.fillRect(itemBounds.getX() - 3.0F,
                                  static_cast<float>(markerStrip.getY() + 2),
                                  7.0F, 5.0F);
                graphics.setFont(WorkbenchType::microStrong());
                graphics.drawText(
                    juceString(item.label),
                    static_cast<int>(std::lround(itemBounds.getX())) + 5,
                    markerStrip.getY(),
                    84,
                    markerStrip.getHeight(),
                    juce::Justification::centredLeft,
                    true);
                continue;
            }
            if (item.kind == TimelineScoreItemKind::AutomationSegment)
            {
                // A slaved lane's graph is a pale
                // version of its master's.
                const auto slavedRow = localRow < viewport_.rows.size() && viewport_.rows[localRow].slaved;
                const auto lineColour = (item.selected ? selectionColour : controlColour)
                    .withAlpha(slavedRow ? 0.3F : 0.9F);
                const auto startY = itemBounds.getBottom()
                    - static_cast<float>(item.displayValue)
                        * itemBounds.getHeight();
                const auto endValue = item.interpolation
                        == talas::core::AutomationInterpolation::Linear
                    ? item.displayEndValue
                    : item.displayValue;
                const auto endY = itemBounds.getBottom()
                    - static_cast<float>(endValue)
                        * itemBounds.getHeight();
                // The area under the line, faintly filled.
                juce::Path under;
                under.startNewSubPath(itemBounds.getX(), itemBounds.getBottom());
                under.lineTo(itemBounds.getX(), startY);
                under.lineTo(itemBounds.getRight(), endY);
                under.lineTo(itemBounds.getRight(), itemBounds.getBottom());
                under.closeSubPath();
                graphics.setGradientFill(juce::ColourGradient{lineColour.withMultipliedAlpha(0.22F), 0.0F,
                    std::min(startY, endY), lineColour.withAlpha(0.0F), 0.0F, itemBounds.getBottom(), false});
                graphics.fillPath(under);
                juce::Path line;
                line.startNewSubPath(itemBounds.getX(), startY);
                line.lineTo(itemBounds.getRight(), endY);
                WorkbenchStyle::strokeWithGlow(graphics, line, lineColour, 1.5F, 5.0F);
                continue;
            }
            if (item.kind == TimelineScoreItemKind::AutomationPoint)
            {
                graphics.setColour(item.selected || pointSelectedWithOthers(item.stableId, item.sourceIndex)
                        ? selectionColour
                        : controlColour);
                const auto valueY = itemBounds.getBottom()
                    - static_cast<float>(item.displayValue)
                        * itemBounds.getHeight();
                graphics.fillEllipse(itemBounds.getCentreX() - 3.5F, valueY - 3.5F,
                                     7.0F, 7.0F);
                graphics.setColour(workbenchTheme.well);
                graphics.fillEllipse(itemBounds.getCentreX() - 1.5F, valueY - 1.5F, 3.0F, 3.0F);
                continue;
            }
            // A Sound selected with others shows as
            // selected too.
            const auto selected = item.selected || (item.kind == TimelineScoreItemKind::Clip && clipSelectedWithOthers(item.stableId));
            // A Sound being cached turns red, a cached one gray.
            const auto cachedColour = item.kind == TimelineScoreItemKind::Clip
                ? talas::app::timelineCachedSoundColour(cacheLookOf(item.stableId))
                : std::nullopt;
            // A Sound the colour menu coloured is drawn in
            // its colour.
            const auto& clips = bridge_.editor().document().timeline.score.clips;
            const auto clip = item.kind == TimelineScoreItemKind::Clip
                ? std::ranges::find(clips, item.stableId, &talas::project::ScoreClip::clipId)
                : clips.end();
            const auto ownColour = clip != clips.end() && clip->colour.has_value()
                ? std::optional<juce::Colour>{juce::Colour{0xFF000000U | *clip->colour}}
                : std::nullopt;
            const auto clipColour = selected
                    ? selectionColour
                    : cachedColour.has_value() ? juce::Colour{*cachedColour}
                    : ownColour.has_value() ? *ownColour
                    : audioColour;
            const auto clipAlpha = selected ? 0.34F : cachedColour.has_value() ? 0.5F : ownColour.has_value() ? 0.42F
                    : (item.overlaps ? 0.44F : 0.3F);
            // A tinted glass body under a lit top band in the clip's colour.
            graphics.setGradientFill(juce::ColourGradient{clipColour.withAlpha(clipAlpha), 0.0F, itemBounds.getY(),
                clipColour.withAlpha(clipAlpha * 0.55F), 0.0F, itemBounds.getBottom(), false});
            graphics.fillRoundedRectangle(itemBounds, 4.0F);
            {
                juce::Graphics::ScopedSaveState clipped(graphics);
                juce::Path body;
                body.addRoundedRectangle(itemBounds, 4.0F);
                graphics.reduceClipRegion(body);
                graphics.setColour(clipColour.withAlpha(0.95F));
                graphics.fillRect(itemBounds.withHeight(3.0F));
            }
            // A marked Sound's outline and text
            // turn light grey.
            const auto marked = item.kind == TimelineScoreItemKind::Clip && marks_.marked(item.stableId);
            const auto outline = talas::app::timelineClipOutline(item.overlaps, selected, marked);
            graphics.setColour(outline == talas::app::TimelineClipLook::Overlapping ? errorColour
                    : outline == talas::app::TimelineClipLook::Selected            ? selectionColour
                    : outline == talas::app::TimelineClipLook::Marked              ? juce::Colours::lightgrey
                                                                                : clipColour.brighter(0.15F));
            graphics.drawRoundedRectangle(
                itemBounds,
                4.0F,
                item.selected ? WorkbenchType::focusOutlinePixels() : 1.0F);
            graphics.setColour(clipColour.brighter(0.3F).withAlpha(0.8F));
            if (item.fadeInFraction > 0.0)
            {
                graphics.drawLine(
                    itemBounds.getX(),
                    itemBounds.getBottom(),
                    itemBounds.getX()
                        + itemBounds.getWidth()
                            * static_cast<float>(item.fadeInFraction),
                    itemBounds.getY(),
                    1.0F);
            }
            if (item.fadeOutFraction > 0.0)
            {
                graphics.drawLine(
                    itemBounds.getRight()
                        - itemBounds.getWidth()
                            * static_cast<float>(item.fadeOutFraction),
                    itemBounds.getY(),
                    itemBounds.getRight(),
                    itemBounds.getBottom(),
                    1.0F);
            }
            if (item.repeatCount > 1U && item.repeatCount <= 16U)
            {
                for (std::uint32_t repeat = 1; repeat < item.repeatCount; ++repeat)
                {
                    const auto repeatX = itemBounds.getX()
                        + itemBounds.getWidth()
                            * static_cast<float>(repeat)
                            / static_cast<float>(item.repeatCount);
                    graphics.drawVerticalLine(
                        static_cast<int>(std::lround(repeatX)),
                        itemBounds.getY() + 3.0F,
                        itemBounds.getBottom() - 3.0F);
                }
            }
            graphics.setFont(WorkbenchType::labelStrong());
            graphics.setColour(talas::app::timelineClipText(marked) == talas::app::TimelineClipLook::Marked ? juce::Colours::lightgrey
                                                                                                 : textColour);
            auto itemLabel = juceString(item.label);
            if (item.kind == TimelineScoreItemKind::Clip
                && clipArtifactState_)
            {
                itemLabel += juceString(" · ")
                    + juceString(clipArtifactState_(item.stableId).label);
            }
            graphics.drawText(
                itemLabel,
                itemBounds.toNearestInt().reduced(6, 2),
                juce::Justification::centredLeft,
                true);
            if (item.repeatCount > 1U && itemBounds.getWidth() > 48.0F)
            {
                graphics.setColour(secondaryTextColour);
                graphics.setFont(WorkbenchType::micro());
                graphics.drawText(
                    juceString("×") + juce::String(item.repeatCount),
                    itemBounds.toNearestInt().reduced(5, 2),
                    juce::Justification::centredRight,
                    false);
            }
        }

        if (viewport_.playheadX.has_value())
        {
            const auto x = timelineArea.getX()
                + static_cast<float>(*viewport_.playheadX);
            // The yellow time cursor, softly lit.
            const auto cursor = juce::Colour{talas::app::timelineTimeCursorColour()};
            if (!WorkbenchStyle::enhancedContrast())
            {
                graphics.setColour(cursor.withAlpha(0.18F));
                graphics.fillRect(juce::Rectangle<float>{x - 2.0F, static_cast<float>(timelineArea.getY()), 5.0F,
                    static_cast<float>(timelineArea.getHeight())});
            }
            graphics.setColour(cursor);
            graphics.drawVerticalLine(
                static_cast<int>(std::lround(x)),
                static_cast<float>(timelineArea.getY()),
                static_cast<float>(timelineArea.getBottom()));
        }
        if (boxStart_.has_value() && boxEnd_.has_value())
        {
            graphics.setColour(selectionColour.withAlpha(0.7F));
            graphics.drawRect(juce::Rectangle<float>{*boxStart_, *boxEnd_}, 1.0F);
        }
        if (viewport_.truncated)
        {
            graphics.setColour(errorColour);
            graphics.setFont(WorkbenchType::microStrong());
            graphics.drawText(
                "Visible density exceeds the fixed projection bound",
                timelineArea.reduced(8).removeFromBottom(18),
                juce::Justification::centredRight);
        }
    }

    void resized() override
    {
        auto bounds = getLocalBounds();
        auto toolbar = bounds.removeFromTop(scoreToolbarHeight());
        timeSource_.setBounds(toolbar.removeFromLeft(std::min(88, std::max(0, toolbar.getWidth() / 6))));
        toolbar.removeFromLeft(6);
        rulerMode_.setBounds(toolbar.removeFromLeft(
            std::min(152, std::max(0, toolbar.getWidth() / 2))));
        toolbar.removeFromLeft(6);
        framesMenu_.setBounds(toolbar.removeFromLeft(std::min(92, std::max(0, toolbar.getWidth() / 6))));
        toolbar.removeFromLeft(6);
        quantization_.setBounds(toolbar.removeFromLeft(std::min(70, std::max(0, toolbar.getWidth() / 4))));
        toolbar.removeFromLeft(6);
        gridMode_.setBounds(toolbar.removeFromLeft(std::min(132, std::max(0, toolbar.getWidth() / 3))));
        toolbar.removeFromLeft(6);
        programChannel_.setBounds(toolbar.removeFromLeft(
            std::min(96, std::max(0, toolbar.getWidth() / 3))));
        toolbar.removeFromLeft(6);
        audioMenu_.setBounds(toolbar.removeFromLeft(std::min(60, std::max(0, toolbar.getWidth() / 4))));
        toolbar.removeFromLeft(6);
        cacheMenu_.setBounds(toolbar.removeFromLeft(std::min(64, std::max(0, toolbar.getWidth() / 4))));
        toolbar.removeFromLeft(6);
        zoom_.setBounds(toolbar.removeFromLeft(
            std::min(158, std::max(0, toolbar.getWidth()))));
        infoStrip_.setBounds(bounds.removeFromTop(scoreToolbarHeight()));
        // The zoom buttons in the lower right corner, below
        // the vertical scroll bar.
        const auto scrolling = talas::app::timelineScoreScrolling(bounds.getWidth(), bounds.getHeight(), scoreScrollBarWidth());
        const auto area = [origin = bounds.getPosition()](const talas::app::TimelineScoreArea& part) {
            return juce::Rectangle<int>{origin.x + part.x, origin.y + part.y, part.width, part.height};
        };
        vertical_.setBounds(area(scrolling.vertical));
        horizontal_.setBounds(area(scrolling.horizontal));
        auto buttons = area(scrolling.zoomButtons);
        for (auto* button : {&zoomOut_, &zoomIn_, &zoomFit_})
            button->setBounds(buttons.removeFromTop(scrolling.zoomButtons.height / 3));
        updateScrollRanges();
        rebuildViewport();
    }

    void mouseDown(const juce::MouseEvent& event) override
    {
        finishOwnedGesture();
        if (isShowing())
            grabKeyboardFocus();
        const auto body = contentBounds();
        const auto rows = body.withTrimmedTop(scoreRulerHeight());
        const auto ruler = body.withHeight(scoreRulerHeight());
        if (ruler.contains(event.getPosition())
            && event.position.x
                >= static_cast<float>(ruler.getX() + scoreHeaderWidth()))
        {
            const auto frame = frameAtPointerX(event.position.x);
            if (event.getNumberOfClicks() >= 2 && frame.has_value())
            {
                createMarkerAtFrame(
                    *frame,
                    event.mods.isCommandDown() || event.mods.isCtrlDown());
            }
            // The strip above the ruler is the markers';
            // the ruler below it places the time cursor.
            else if (event.position.y < static_cast<float>(ruler.getY() + scoreStripHeight()))
            {
                onStatus_("Double-click in the marker strip to add a marker");
            }
            else if (runtime_.syncKind != TimelineExternalSyncKind::Internal)
            {
                onStatus_("Seek is held while an external transport owns position");
            }
            else if (frame.has_value()
                && bridge_.requestTransportRelocation(*frame))
            {
                onStatus_("Queued exact Timeline seek at the next audio boundary");
            }
            else
            {
                onStatus_("Timeline seek could not be queued");
            }
            return;
        }
        if (!rows.contains(event.getPosition()))
            return;
        const auto localY = event.position.y - static_cast<float>(rows.getY());
        const auto timelineX = event.position.x - static_cast<float>(
            rows.getX() + scoreHeaderWidth());
        const auto hit = projection_ != nullptr && timelineX >= 0.0F
            ? projection_->hitTest(viewport_, timelineX, localY)
            : std::optional<TimelineScoreHit>{};
        const auto rowOffset = static_cast<std::size_t>(std::max(
            0,
            static_cast<int>(localY) / scoreRowHeight()));
        // A click on a track chooses where Paste
        // puts the Sounds copied, at the time clicked.
        if (rowOffset < viewport_.rows.size() && viewport_.rows[rowOffset].kind == TimelineScoreRowKind::Track
            && timelineX >= 0.0F)
        {
            pasteTrackId_ = viewport_.rows[rowOffset].stableId;
            pasteFrame_ = frameAtPointerX(event.position.x);
        }
        if (!hit.has_value() && rowOffset >= viewport_.rows.size())
        {
            if (event.mods.isPopupMenu())
            {
                selection_ = {};
                selectedFocusIdentity_.clear();
                rebuildViewport();
                contextPointerFrame_ = frameAtPointerX(event.position.x);
                showContextMenu();
            }
            return;
        }
        if (pencilMode_ && rowOffset < viewport_.rows.size()
            && viewport_.rows[rowOffset].kind == TimelineScoreRowKind::Automation
            && event.position.x >= static_cast<float>(rows.getX() + scoreHeaderWidth()))
        {
            // The pencil: a stroke over an automation lane.
            const auto& row = viewport_.rows[rowOffset];
            selection_ = {TimelineScoreSelectionKind::AutomationLane, row.stableId, 0};
            selectedFocusIdentity_ = rowFocusIdentity(row);
            strokeLaneId_ = row.stableId;
            strokeRowTop_ = static_cast<float>(rows.getY() + static_cast<int>(rowOffset) * scoreRowHeight());
            stroke_.clear();
            appendStrokePoint(event.position);
            return;
        }
        // A double-click on a Sound opens its editor.
        if (event.getNumberOfClicks() >= 2 && hit.has_value() && hit->selection.kind == TimelineScoreSelectionKind::Clip
            && !event.mods.isAnyModifierKeyDown())
        {
            if (editClipSound(hit->selection.entityId))
                return;
        }
        // Shift+click adds a Sound to the selected
        // group, or takes it out.
        if (event.mods.isShiftDown() && hit.has_value() && hit->selection.kind == TimelineScoreSelectionKind::Clip
            && selection_.kind == TimelineScoreSelectionKind::Clip)
        {
            toggleSelectedClip(hit->selection.entityId);
            rebuildViewport();
            repaint();
            return;
        }
        // Shift+click adds a point of the selected
        // point's lane to the selection, or takes it out.
        if (event.mods.isShiftDown() && hit.has_value()
            && hit->selection.kind == TimelineScoreSelectionKind::AutomationPoint
            && selection_.kind == TimelineScoreSelectionKind::AutomationPoint
            && hit->selection.entityId == selection_.entityId)
        {
            toggleSelectedPoint(hit->selection.pointIndex);
            rebuildViewport();
            repaint();
            return;
        }
        const auto inLane = !hit.has_value() && rowOffset < viewport_.rows.size()
            && viewport_.rows[rowOffset].kind == TimelineScoreRowKind::Automation
            && event.position.x >= static_cast<float>(rows.getX() + scoreHeaderWidth())
            && !event.mods.isPopupMenu();
        // Ctrl+Click (Command on the Mac) adds a point at the time and
        // value clicked.
        if (inLane && event.mods.isCommandDown() && event.getNumberOfClicks() == 1)
        {
            const auto& row = viewport_.rows[rowOffset];
            selection_ = {TimelineScoreSelectionKind::AutomationLane, row.stableId, 0};
            selectedFocusIdentity_ = rowFocusIdentity(row);
            clearExtraPoints();
            const auto top = static_cast<float>(rows.getY() + static_cast<int>(rowOffset) * scoreRowHeight() + 4);
            const auto height = 1.0F - (event.position.y - top) / static_cast<float>(std::max(1, scoreRowHeight() - 8));
            if (const auto frame = frameAtPointerX(event.position.x))
                (void) addAutomationPointAtFrame(*frame, false, static_cast<double>(height));
            return;
        }
        // Option-click marks a Sound to play it
        // alone; with Shift it toggles the mark.
        if (event.mods.isAltDown() && hit.has_value() && hit->selection.kind == TimelineScoreSelectionKind::Clip)
        {
            (void) markClip(hit->selection.entityId, event.mods.isShiftDown());
            return;
        }
        if (event.getNumberOfClicks() >= 2
            && !hit.has_value()
            && event.position.x
                >= static_cast<float>(rows.getX() + scoreHeaderWidth()))
        {
            const auto& row = viewport_.rows[rowOffset];
            selection_ = {
                row.kind == TimelineScoreRowKind::Automation
                    ? TimelineScoreSelectionKind::AutomationLane
                    : TimelineScoreSelectionKind::Track,
                row.stableId,
                0,
            };
            selectedFocusIdentity_ = rowFocusIdentity(row);
            const auto frame = frameAtPointerX(event.position.x);
            if (!frame.has_value())
                return;
            if (row.kind == TimelineScoreRowKind::Automation)
                addAutomationPointAtFrame(
                    *frame,
                    event.mods.isCommandDown() || event.mods.isCtrlDown());
            else
                createClipAtFrame(
                    row.stableId,
                    *frame,
                    event.mods.isCommandDown() || event.mods.isCtrlDown());
            return;
        }
        if (event.position.x < static_cast<float>(rows.getX() + scoreHeaderWidth()))
        {
            const auto& row = viewport_.rows[rowOffset];
            selection_ = {
                row.kind == TimelineScoreRowKind::Automation
                    ? TimelineScoreSelectionKind::AutomationLane
                    : TimelineScoreSelectionKind::Track,
                row.stableId,
                0,
            };
            selectedFocusIdentity_ = rowFocusIdentity(row);
        }
        else if (projection_)
        {
            if (hit.has_value())
            {
                selection_ = hit->selection;
                selectedFocusIdentity_ = hit->focusIdentity;
                captureSelectedAutomationPoint();
            }
            else
            {
                selection_ = {};
                selectedFocusIdentity_.clear();
                selectedPointPosition_.reset();
            }
        }
        clearExtraPoints();
        // A box drawn over a lane selects the points in it;
        // one drawn over the tracks, the Sounds it touches.
        const auto onEmptyTrack = !hit.has_value() && rowOffset < viewport_.rows.size()
            && viewport_.rows[rowOffset].kind == TimelineScoreRowKind::Track
            && event.position.x >= static_cast<float>(rows.getX() + scoreHeaderWidth()) && !event.mods.isPopupMenu();
        if ((inLane || onEmptyTrack) && event.getNumberOfClicks() == 1 && !event.mods.isAnyModifierKeyDown())
        {
            boxStart_ = event.position;
            boxEnd_.reset();
            boxRow_ = firstVisibleRow_ + rowOffset;
            boxForClips_ = onEmptyTrack;
        }
        rebuildViewport();
        updateAccessibleTitle();
        focusAccessibleEntity(selectedFocusIdentity_);
        preparePointerGesture(event);
        if (event.mods.isPopupMenu())
        {
            contextPointerFrame_ = frameAtPointerX(event.position.x);
            showContextMenu();
            return;
        }
        if (selection_.kind != TimelineScoreSelectionKind::None)
            onStatus_("Selected Score " + juceString(selection_.entityId));
        repaint();
    }

    // A Sound dragged from the Sound browser onto a track becomes a clip
    // there.
    [[nodiscard]] static std::string draggedSoundDefinitionId(const juce::var& description)
    {
        if (!description.isString())
            return {};
        const auto value = description.toString().toStdString();
        return value.starts_with(soundDragPrefix)
            ? value.substr(soundDragPrefix.size())
            : std::string{};
    }

    [[nodiscard]] const TimelineScoreRowProjection* trackRowAt(juce::Point<int> position) const
    {
        const auto rows = contentBounds().withTrimmedTop(scoreRulerHeight());
        if (!rows.contains(position))
            return nullptr;
        const auto offset = static_cast<std::size_t>(
            std::max(0, (position.y - rows.getY()) / scoreRowHeight()));
        if (offset >= viewport_.rows.size())
            return nullptr;
        const auto& row = viewport_.rows[offset];
        return row.kind == TimelineScoreRowKind::Track ? &row : nullptr;
    }

    [[nodiscard]] const TimelineScoreRowProjection* automationRowAt(juce::Point<int> position) const
    {
        const auto rows = contentBounds().withTrimmedTop(scoreRulerHeight());
        if (!rows.contains(position))
            return nullptr;
        const auto offset = static_cast<std::size_t>(std::max(0, (position.y - rows.getY()) / scoreRowHeight()));
        return offset < viewport_.rows.size() && viewport_.rows[offset].kind == TimelineScoreRowKind::Automation
            ? &viewport_.rows[offset]
            : nullptr;
    }

    // A Sound or a file from a Sound Browser, a Sound out of
    // a signal flow, or a prototype, joins the project's Sounds as it is
    // dropped; a Sound of the project is itself.
    [[nodiscard]] std::string definitionDragged(const juce::var& description)
    {
        const auto text = description.toString().toStdString();
        if ((talas::app::soundBrowserDrag(text).has_value() || talas::app::signalFlowSoundDrag(text).has_value()
                || !draggedPrototypeTypeId(description).empty())
            && dragImporter_)
            return dragImporter_(text).value_or(std::string{});
        return draggedSoundDefinitionId(description);
    }

    [[nodiscard]] static std::string draggedPrototypeTypeId(const juce::var& description)
    {
        if (!description.isString())
            return {};
        const auto value = description.toString().toStdString();
        return value.starts_with(prototypeDragPrefix) ? value.substr(prototypeDragPrefix.size()) : std::string{};
    }

    // The clip under a point of the Timeline, if any.
    [[nodiscard]] std::optional<std::string> clipAt(juce::Point<int> position) const
    {
        const auto rows = contentBounds().withTrimmedTop(scoreRulerHeight());
        if (!rows.contains(position) || projection_ == nullptr)
            return std::nullopt;
        const auto hit = projection_->hitTest(viewport_,
            static_cast<float>(position.x - (rows.getX() + scoreHeaderWidth())),
            static_cast<float>(position.y - rows.getY()));
        if (!hit.has_value() || hit->selection.kind != TimelineScoreSelectionKind::Clip)
            return std::nullopt;
        return hit->selection.entityId;
    }

    bool isInterestedInDragSource(const SourceDetails& details) override
    {
        if (const auto text = details.description.toString().toStdString(); !draggedPrototypeTypeId(details.description).empty()
            || talas::app::soundBrowserDrag(text).has_value() || talas::app::signalFlowSoundDrag(text).has_value())
            return true;
        const auto definitionId = draggedSoundDefinitionId(details.description);
        const auto& definitions = bridge_.editor().document().soundDefinitions;
        return !definitionId.empty()
            && std::ranges::any_of(definitions, [&](const auto& definition) {
                   return definition.definitionId == definitionId;
               });
    }

    void itemDropped(const SourceDetails& details) override
    {
        const auto clip = clipAt(details.localPosition);
        // An effect dropped on a clip; a Sound dropped on
        // a clip with Option (Control) replaces its input.
        // On a track, a prototype is a new Sound there.
        if (const auto typeId = draggedPrototypeTypeId(details.description); !typeId.empty() && clip.has_value())
        {
            (void)applyEffectToClip(*clip, typeId);
            return;
        }
        const auto definitionId = definitionDragged(details.description);
        if (definitionId.empty())
            return;
        const auto modifiers = juce::ModifierKeys::getCurrentModifiers();
        if (clip.has_value() && (modifiers.isAltDown() || modifiers.isCtrlDown()))
        {
            (void)replaceClipInput(*clip, definitionId);
            return;
        }
        // A Sound with an Input, from the Sound Browser, a Sound
        // file window or a Sound editor, dropped on a clip is its effect.
        if (const auto& sounds = bridge_.editor().document().soundDefinitions;
            clip.has_value()
            && std::ranges::any_of(sounds, [&](const talas::project::ProjectSoundDefinition& sound) {
                   return sound.definitionId == definitionId
                       && std::ranges::any_of(sound.inputs, [](const talas::project::ProjectSoundPort& port) {
                              return port.type == talas::core::PortType::Audio;
                          });
               }))
        {
            selection_ = {TimelineScoreSelectionKind::Clip, *clip, 0};
            (void)commitDiscreteEdit(bridge_.editor().applySoundEffectToScoreClip(*clip, definitionId),
                "Applied the Sound to the clip as its effect");
            return;
        }
        const auto* row = trackRowAt(details.localPosition);
        const auto* lane = automationRowAt(details.localPosition);
        const auto frame = frameAtPointerX(static_cast<float>(details.localPosition.x));
        using Target = talas::app::TimelineDropTarget;
        switch (talas::app::timelineDropTarget(rowUnder(details.localPosition), row != nullptr, lane != nullptr, viewport_.rows.size()))
        {
        case Target::Lane:
            // A Sound dropped into a controller's lane.
            askForEnvelope(lane->stableId, definitionId);
            return;
        case Target::Track:
            if (frame.has_value())
            {
                (void)dropSoundOnTrack(definitionId, row->stableId, *frame);
                return;
            }
            break;
        case Target::NewTrack:
            // Below the last track, a new track for the Sound.
            if (frame.has_value())
            {
                (void)dropSoundOnNewTrack(definitionId, *frame);
                return;
            }
            break;
        case Target::None:
            break;
        }
        onStatus_("Drop a Sound on a track to add a clip there");
    }

    // The row under a point, counted from the first row shown; past the
    // last row for a point below them.
    [[nodiscard]] std::optional<std::size_t> rowUnder(juce::Point<int> position) const
    {
        const auto rows = contentBounds().withTrimmedTop(scoreRulerHeight());
        if (!rows.contains(position))
            return std::nullopt;
        return static_cast<std::size_t>(std::max(0, (position.y - rows.getY()) / scoreRowHeight()));
    }

    bool dropSoundOnNewTrack(std::string_view definitionId, std::uint64_t frame)
    {
        const auto tracks = bridge_.editor().document().timeline.score.tracks.size();
        if (!addScoreTrack() || bridge_.editor().document().timeline.score.tracks.size() <= tracks
            || selection_.kind != TimelineScoreSelectionKind::Track)
            return false;
        const auto trackId = selection_.entityId;
        return dropSoundOnTrack(definitionId, trackId, frame);
    }

    bool applyEffectToClip(std::string_view clipId, std::string_view typeId)
    {
        selection_ = {TimelineScoreSelectionKind::Clip, std::string{clipId}, 0};
        return commitDiscreteEdit(
            bridge_.editor().applyEffectToScoreClip(clipId, std::string{typeId}), "Applied the effect to the clip");
    }

    bool replaceClipInput(std::string_view clipId, std::string_view definitionId)
    {
        selection_ = {TimelineScoreSelectionKind::Clip, std::string{clipId}, 0};
        return commitDiscreteEdit(
            bridge_.editor().replaceScoreClipInput(clipId, definitionId), "Replaced the clip's input");
    }

    bool dropSoundOnTrack(
        std::string_view definitionId,
        std::string_view trackId,
        std::uint64_t frame)
    {
        selection_ = {TimelineScoreSelectionKind::Track, std::string{trackId}, 0};
        return createClipAtFrame(trackId, frame, false, definitionId);
    }

    bool editClipSound(std::string_view clipId)
    {
        const auto& clips = bridge_.editor().document().timeline.score.clips;
        const auto clip = std::ranges::find(clips, clipId, &talas::project::ScoreClip::clipId);
        if (clip == clips.end() || !soundToEdit_)
            return false;
        soundToEdit_(clip->soundDefinitionId);
        return true;
    }

    // A transformation button across the
    // bottom of the controls editor pressed for a control: it asks for what
    // it needs, as the context menu's item does; pressed on the control
    // already, it takes the transformation away.
    bool pressTransformationButton(const std::string& laneId, talas::app::AutomationTransform transform)
    {
        const auto& lanes = bridge_.editor().document().timeline.score.automation;
        const auto lane = std::ranges::find(lanes, laneId, &talas::project::ScoreAutomationLane::laneId);
        if (lane == lanes.end() || lane->points.empty())
        {
            onStatus_("Select a control with points to transform");
            return false;
        }
        selection_ = {TimelineScoreSelectionKind::AutomationLane, laneId, 0};
        using Transform = talas::app::AutomationTransform;
        switch (transform)
        {
        case Transform::Reverse:
        case Transform::SquareOff:
            return transformSelectedAutomationLane(transform, {});
        case Transform::ScaleToFit:
            return scaleSelectedAutomationLaneToItsClip();
        case Transform::Invert:
            promptAutomationTransform(transformInvert);
            return true;
        case Transform::Loop:
            promptAutomationTransform(transformLoop);
            return true;
        case Transform::Scale:
            promptAutomationTransform(transformScale);
            return true;
        case Transform::Offset:
            promptAutomationTransform(transformOffset);
            return true;
        case Transform::Threshold:
            promptAutomationTransform(transformThreshold);
            return true;
        case Transform::TimeScale:
            promptAutomationTransform(transformTimeScale);
            return true;
        case Transform::Spray:
            promptAutomationTransform(transformSpray);
            return true;
        case Transform::QuantizeEndpoints:
            promptAutomationTransform(transformQuantize);
            return true;
        }
        return false;
    }

    // Copy and Paste: the selected Sounds
    // with their own controls; pasted on the track clicked last at the time
    // clicked there, else on the first one's own track at the cursor.
    bool copySelectedClips()
    {
        keepCopiedClips(talas::app::timelineClipsCopied(bridge_.editor().document().timeline.score, selectedClipIds()));
        if (soundsCopied_ && !clipsCopied_.clips.empty())
            soundsCopied_(talas::app::timelineCopiedSounds(clipsCopied_));
        onStatus_(clipsCopied_.clips.empty() ? juceString("Select the Sounds to copy")
                                             : "Copied " + juce::String{static_cast<int>(clipsCopied_.clips.size())}
                    + " Sound" + (clipsCopied_.clips.size() == 1 ? "" : "s"));
        return true;
    }

    bool pasteCopiedClips()
    {
        // A Sound copied elsewhere, in a signal flow, is
        // pasted as a new clip on the track clicked.
        if (clipsCopied_.clips.empty())
        {
            const auto sound = soundToPaste_ ? soundToPaste_() : std::nullopt;
            if (!sound.has_value() || pasteTrackId_.empty())
            {
                onStatus_("Copy a Sound, and click a track, to paste it there");
                return false;
            }
            return createClipAtFrame(pasteTrackId_, pasteFrame_.value_or(runtime_.playheadFrame), false, *sound);
        }
        const auto trackId = pasteTrackId_.empty() ? clipsCopied_.clips.front().trackId : pasteTrackId_;
        const auto at = timeAtFrame(pasteFrame_.value_or(runtime_.playheadFrame), clipsCopied_.clips.front().start.domain, false);
        // Copied in another Timeline, the clips
        // bring the Sounds they play; within this one nothing comes in.
        if (clipsCopiedFrom_ != nullptr && pasteRegistry_ != nullptr && at.has_value())
        {
            const auto& current = bridge_.editor().document();
            auto pasted = talas::app::timelineClipsPastedFromAnotherTimeline(current, *clipsCopiedFrom_, clipsCopied_, trackId,
                *at, *pasteRegistry_, clipsCopiedFromFile_, projectFile_ ? projectFile_() : std::nullopt);
            if (!pasted.ok())
            {
                onStatus_(juceString(pasted.diagnostic));
                return true;
            }
            if (pasted.document->soundDefinitions == current.soundDefinitions && pasted.document->assets == current.assets)
                return commitDiscreteEdit(bridge_.editor().applyScoreEdits(std::move(pasted.batch)), "Pasted the Sounds copied");
            return commitDiscreteEdit(bridge_.editor().applyDocumentEdit(std::move(*pasted.document)),
                "Pasted the Sounds copied in another Timeline, with the Sounds they play");
        }
        const auto batch = at.has_value()
            ? talas::app::timelineClipsPasted(bridge_.editor().document().timeline.score, clipsCopied_, trackId, *at)
            : std::nullopt;
        if (!batch.has_value())
        {
            onStatus_("The Sounds copied cannot be pasted there");
            return true;
        }
        return commitDiscreteEdit(bridge_.editor().applyScoreEdits(*batch), "Pasted the Sounds copied");
    }

    // A lane's vertical zoom, steps out
    // (more of the range of values shown) or in (more detail).
    [[nodiscard]] int laneValueZoom(std::string_view laneId) const
    {
        const auto found = std::ranges::find(laneValueZooms_, laneId, &std::pair<std::string, int>::first);
        return found == laneValueZooms_.end() ? 0 : found->second;
    }

    bool zoomLaneValues(const std::string& laneId, int steps)
    {
        const auto& lanes = bridge_.editor().document().timeline.score.automation;
        if (std::ranges::find(lanes, laneId, &talas::project::ScoreAutomationLane::laneId) == lanes.end())
            return false;
        const auto zoom = std::clamp(laneValueZoom(laneId) + steps, -8, 8);
        std::erase_if(laneValueZooms_, [&](const auto& entry) { return entry.first == laneId; });
        if (zoom != 0)
            laneValueZooms_.emplace_back(laneId, zoom);
        rebuild(bridge_.editor().document(), runtime_, true);
        onStatus_(steps > 0 ? juceString("Zoomed the lane out to show more of its range of values")
                            : juceString("Zoomed the lane in to show its values in more detail"));
        return true;
    }

    // The Timeline is a Sound; its equivalent, a
    // Mixer of SetDurations, joins the project's Sounds.
    bool makeSoundOfTimeline()
    {
        const auto& document = bridge_.editor().document();
        std::string name = "Timeline";
        for (int number = 1; std::ranges::any_of(document.soundDefinitions, [&](const auto& sound) {
                 return sound.definitionId == name || sound.displayName == name;
             });
             ++number)
            name = "Timeline" + std::to_string(number);
        auto sound = talas::app::timelineAsSound(document, name);
        if (!sound.has_value())
        {
            onStatus_("The Timeline has no Sound to make");
            return false;
        }
        const auto result = bridge_.editor().addSoundDefinition(std::move(*sound));
        if (!result.ok())
        {
            onStatus_(juceString(result.message));
            return false;
        }
        onDocumentChange_();
        onStatus_("Made the Timeline's Sound " + juceString(name) + ", a Mixer of SetDurations");
        return true;
    }

#if defined(TALAS_WORKBENCH_TIMELINE_TEST_SUPPORT)
    [[nodiscard]] std::string infoFieldTextForSmoke(TimelineScoreSelection selection, TimelineInfoField field)
    {
        selection_ = std::move(selection);
        refreshInfoStrip();
        return infoStrip_.fieldText(field);
    }

    [[nodiscard]] bool enterInfoFieldForSmoke(TimelineScoreSelection selection, TimelineInfoField field, const std::string& text)
    {
        selection_ = std::move(selection);
        refreshInfoStrip();
        return infoStrip_.enterField(field, text);
    }

    [[nodiscard]] std::optional<TimelineInfoField> typingInfoFieldForSmoke() const { return infoStrip_.typing(); }

    [[nodiscard]] std::string typeIntoInfoFieldForSmoke(
        TimelineScoreSelection selection, TimelineInfoField field, const juce::String& keys)
    {
        selection_ = std::move(selection);
        refreshInfoStrip();
        infoStrip_.beginTyping(field);
        for (const auto character : keys)
            (void) infoStrip_.pressKey(field, juce::KeyPress{static_cast<int>(character), {}, character});
        return infoStrip_.fieldText(field);
    }

    [[nodiscard]] bool setMarkerProgramForSmoke(TimelineScoreSelection selection, std::uint8_t program)
    {
        selection_ = std::move(selection);
        return setSelectedMarkerProgram(program);
    }

    [[nodiscard]] bool setMarkerProgramChannelForSmoke(std::uint8_t channel)
    {
        return setMarkerProgramChannel(channel);
    }

    [[nodiscard]] bool anchorAtCursorForSmoke(TimelineScoreSelection selection)
    {
        selection_ = std::move(selection);
        return anchorSelectedClipAtCursor();
    }

    [[nodiscard]] bool captureMarkerForSmoke()
    {
        return createMarkerAtPlayhead();
    }

    [[nodiscard]] bool copyAndPasteClipsForSmoke(
        const std::vector<std::string>& clipIds, const std::string& trackId, std::uint64_t frame)
    {
        keepCopiedClips(talas::app::timelineClipsCopied(bridge_.editor().document().timeline.score, clipIds));
        pasteTrackId_ = trackId;
        pasteFrame_ = frame;
        return (!clipIds.empty() ? !clipsCopied_.clips.empty() : true) && pasteCopiedClips();
    }

    [[nodiscard]] bool alignClipForSmoke(TimelineScoreSelection selection, std::string_view anchoredClipId, bool end)
    {
        selection_ = std::move(selection);
        return alignSelectedClip(anchoredClipId,
            end ? talas::app::ClipAlignment::EndToAnchor : talas::app::ClipAlignment::StartToAnchor);
    }

    [[nodiscard]] bool applyEffectToClipForSmoke(std::string_view clipId, std::string_view typeId)
    {
        return applyEffectToClip(clipId, typeId);
    }

    [[nodiscard]] bool replaceClipInputForSmoke(std::string_view clipId, std::string_view definitionId)
    {
        return replaceClipInput(clipId, definitionId);
    }

    [[nodiscard]] bool removeClipEffectForSmoke(std::string_view clipId)
    {
        selection_ = {TimelineScoreSelectionKind::Clip, std::string{clipId}, 0};
        return removeSelectedClipEffect();
    }

    [[nodiscard]] bool drawAutomationStrokeForSmoke(
        std::string_view laneId, std::vector<std::pair<std::uint64_t, double>> stroke)
    {
        return drawAutomationStroke(laneId, std::move(stroke));
    }

    [[nodiscard]] bool dropSoundOnTrackForSmoke(
        std::string_view definitionId,
        std::string_view trackId,
        std::uint64_t frame)
    {
        return dropSoundOnTrack(definitionId, trackId, frame);
    }

    [[nodiscard]] bool dropSoundBelowTracksForSmoke(std::string_view definitionId, std::uint64_t frame)
    {
        return dropSoundOnNewTrack(definitionId, frame);
    }

    [[nodiscard]] bool dropDescriptionOnTrackForSmoke(const juce::String& description, std::string_view trackId, std::uint64_t frame)
    {
        const auto definitionId = definitionDragged(description);
        return !definitionId.empty() && dropSoundOnTrack(definitionId, trackId, frame);
    }

    [[nodiscard]] std::vector<juce::String> automateMenuForSmoke(
        TimelineScoreSelection selection)
    {
        selection_ = std::move(selection);
        std::vector<juce::String> labels;
        for (const auto& target : selectedAutomationTargets())
            labels.push_back(automationTargetLabel(target));
        return labels;
    }

    [[nodiscard]] bool automateForSmoke(
        TimelineScoreSelection selection,
        std::string_view eventName)
    {
        selection_ = std::move(selection);
        const auto owner = selectedAutomationOwner();
        const auto targets = selectedAutomationTargets();
        const auto target = std::ranges::find(
            targets, eventName, &talas::app::AutomationTarget::eventName);
        return owner.has_value() && target != targets.end()
            && addAutomationLaneFor(owner->scope, owner->ownerId, *target)
            && selection_.kind == TimelineScoreSelectionKind::AutomationLane;
    }

    [[nodiscard]] bool shapeAutomationForSmoke(
        TimelineScoreSelection selection,
        talas::app::AutomationCurveShape shape)
    {
        selection_ = std::move(selection);
        return shapeSelectedAutomationSegment(shape);
    }

    [[nodiscard]] bool transformAutomationForSmoke(
        TimelineScoreSelection selection,
        talas::app::AutomationTransform transform,
        talas::app::AutomationTransformParameters parameters)
    {
        selection_ = std::move(selection);
        return transformSelectedAutomationLane(transform, parameters);
    }

    [[nodiscard]] bool setControllerRecordingTakeForSmoke(
        talas::project::ControllerRecordingTake take)
    {
        if (!takeTargetsMatchRecordingLanes(take))
            return false;
        armedControllerRecordingTake_.reset();
        controllerRecordingPreview_.reset();
        controllerRecordingTake_ = std::move(take);
        return true;
    }

    [[nodiscard]] bool invokeLaneStateMenuActionForSmoke(
        TimelineScoreSelection selection, int actionId)
    {
        selection_ = std::move(selection);
        return performLaneStateMenuAction(actionId);
    }

    [[nodiscard]] TimelineControllerRecordingMenuState
    controllerRecordingMenuStateForSmoke()
    {
        refreshControllerRecordingState();
        return controllerRecordingMenuState();
    }

    [[nodiscard]] bool invokeControllerRecordingMenuActionForSmoke(
        int actionId)
    {
        refreshControllerRecordingState();
        return performControllerRecordingMenuAction(actionId);
    }

    [[nodiscard]] TimelineScoreClipArtifactState
    clipArtifactStateForSmoke(std::string_view clipId)
    {
        return clipArtifactState_
            ? clipArtifactState_(clipId)
            : TimelineScoreClipArtifactState{};
    }

    [[nodiscard]] bool invokeClipArtifactActionForSmoke(
        std::string clipId,
        TimelineScoreClipArtifactAction action)
    {
        if (!clipArtifactAction_)
            return false;
        clipArtifactAction_(std::move(clipId), action);
        return true;
    }

    [[nodiscard]] talas::app::TimelineCacheLook cacheLookForSmoke(std::string_view clipId)
    {
        return cacheLookOf(clipId);
    }

    [[nodiscard]] juce::Colour cacheMenuColourForSmoke()
    {
        updateCacheMenuLook();
        return cacheMenu_.findColour(juce::TextButton::buttonColourId);
    }
#endif

    void mouseDrag(const juce::MouseEvent& event) override
    {
        if (!strokeLaneId_.empty())
        {
            appendStrokePoint(event.position);
            return;
        }
        if (boxStart_.has_value())
        {
            boxEnd_ = event.position;
            repaint();
            return;
        }
        if (gestureMode_ == GestureMode::None
            || !event.mouseWasDraggedSinceMouseDown())
        {
            return;
        }
        if (!gestureToken_.has_value())
            gestureToken_ = bridge_.editor().beginOwnedHistoryTransaction();
        const auto pointerFrame = frameAtPointerX(event.position.x);
        if (!pointerFrame.has_value())
            return;
        const auto targetFrame = shiftedFrame(
            gestureEntityFrame_, gesturePointerFrame_, *pointerFrame);
        // A point follows the pointer in value too; Shift held
        // as the drag starts keeps it to the direction first taken.
        std::optional<double> heightMoved;
        auto moveTime = true;
        if (gestureLane_.has_value())
        {
            if (!gestureAxis_.has_value())
                gestureAxis_ = talas::app::automationDragAxis(event.mods.isShiftDown(),
                    static_cast<double>(event.position.x - gestureStartPosition_.x),
                    static_cast<double>(event.position.y - gestureStartPosition_.y));
            if (*gestureAxis_ != talas::app::AutomationDragAxis::Time)
                heightMoved = static_cast<double>(gestureStartPosition_.y - event.position.y)
                    / static_cast<double>(std::max(1, scoreRowHeight() - 8));
            moveTime = *gestureAxis_ != talas::app::AutomationDragAxis::Value;
        }
        applyPointerGesture(
            targetFrame,
            event.mods.isCommandDown() || event.mods.isCtrlDown(),
            heightMoved,
            moveTime);
    }

    void mouseMove(const juce::MouseEvent& event) override
    {
        const auto frame = frameAtPointerX(event.position.x);
        if (!frame.has_value())
        {
            if (lastSnapPreviewFrame_.has_value())
            {
                lastSnapPreviewFrame_.reset();
                onStatus_("Timeline snap candidate cleared outside the exact time range");
            }
            return;
        }
        if (*frame == lastSnapPreviewFrame_)
            return;
        const auto pointer = timelineScorePointerTime(
            *frame,
            selectedTimeDomain(),
            bridge_.editor().document().timeline.musicalTime,
            runtime_.sampleRate);
        lastSnapPreviewFrame_ = *frame;
        if (pointer.has_value() && pointer->snapCandidate.has_value()
            && pointer->snapFrame.has_value()
            && *pointer->snapFrame != pointer->exactFrame)
        {
            onStatus_(
                "Timeline snap candidate frame "
                + unsignedText(*pointer->snapFrame)
                + juceString(" · hold Command to accept; exact frame ")
                + unsignedText(pointer->exactFrame));
        }
        else
        {
            onStatus_(
                "Timeline exact frame " + unsignedText(*frame)
                + juceString(" · no separate snap candidate"));
        }
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        if (boxStart_.has_value())
        {
            const auto start = *boxStart_;
            const auto end = boxEnd_;
            boxStart_.reset();
            boxEnd_.reset();
            if (end.has_value())
                (void) (boxForClips_ ? selectClipsInBox(juce::Rectangle<float>{start, *end})
                                     : selectPointsInBox(boxRow_, juce::Rectangle<float>{start, *end}));
            repaint();
            return;
        }
        if (!strokeLaneId_.empty())
        {
            auto laneId = std::move(strokeLaneId_);
            strokeLaneId_.clear();
            (void)drawAutomationStroke(laneId, std::move(stroke_));
            stroke_.clear();
            return;
        }
        finishOwnedGesture();
    }

    void setPencilMode(bool enabled)
    {
        pencilMode_ = enabled;
        onStatus_(enabled ? "Pencil: drag over an automation lane to draw its function"
                          : "Pencil off");
    }

    // A pointer over the stroke's lane as a frame and a value: the lane's
    // span of values, at least 0 to 1, from the row's foot to its head.
    void appendStrokePoint(juce::Point<float> position)
    {
        const auto frame = frameAtPointerX(position.x);
        if (!frame.has_value())
            return;
        const auto& score = bridge_.editor().document().timeline.score;
        const auto lane = std::ranges::find(score.automation, strokeLaneId_, &talas::project::ScoreAutomationLane::laneId);
        auto low = 0.0;
        auto high = 1.0;
        if (lane != score.automation.end())
        {
            for (const auto& point : lane->points)
            {
                low = std::min(low, point.value);
                high = std::max(high, point.value);
            }
        }
        const auto proportion = std::clamp(
            1.0 - static_cast<double>(position.y - strokeRowTop_) / std::max(1, scoreRowHeight()), 0.0, 1.0);
        stroke_.emplace_back(*frame, low + proportion * (high - low));
    }

    // Draws a stroke of (frame, value) points into a lane, its times in the
    // lane's own domain (a clip's lane from the clip's start).
    bool drawAutomationStroke(std::string_view laneId, std::vector<std::pair<std::uint64_t, double>> stroke)
    {
        const auto& score = bridge_.editor().document().timeline.score;
        const auto found = std::ranges::find(score.automation, laneId, &talas::project::ScoreAutomationLane::laneId);
        if (found == score.automation.end() || stroke.empty())
            return false;
        auto domain = found->points.empty() ? talas::project::ScoreTimeDomain::Seconds : found->points.front().position.domain;
        std::optional<talas::project::ScoreTime> clipStart;
        if (found->scope == talas::project::ScoreAutomationScope::Clip)
        {
            const auto owner = std::ranges::find(score.clips, found->ownerId, &talas::project::ScoreClip::clipId);
            if (owner == score.clips.end())
                return false;
            domain = owner->start.domain;
            clipStart = owner->start;
        }
        else if (found->points.empty())
        {
            const auto owner = std::ranges::find(score.tracks, found->ownerId, &talas::project::ScoreTrack::trackId);
            if (owner != score.tracks.end())
                domain = owner->timeDomain;
        }
        std::vector<talas::app::AutomationStrokePoint> points;
        for (const auto& [frame, value] : stroke)
        {
            auto position = timeAtFrame(frame, domain, false);
            if (position.has_value() && clipStart.has_value())
                position = subtractScoreTime(*position, *clipStart);
            if (!position.has_value())
                continue;
            points.push_back({position->domain == talas::project::ScoreTimeDomain::Seconds
                    ? position->seconds
                    : static_cast<double>(position->tick),
                value});
        }
        // Points closer than about two pixels of the view are one.
        const auto spacing = domain == talas::project::ScoreTimeDomain::Seconds ? 0.005 : 1.0;
        const auto drawn = talas::app::drawAutomationStroke(*found, points, spacing);
        if (!drawn.has_value())
        {
            onStatus_("The stroke could not be drawn into the lane");
            return false;
        }
        if (!laneProjectsAtActiveRate(*drawn))
        {
            onStatus_("The stroke exceeds the active Timeline sample range");
            return false;
        }
        return commitDiscreteEdit(bridge_.editor().updateScoreAutomationLane(*drawn), "Drew the automation function");
    }

    // Minus removes the selected clip's last effect.
    bool removeSelectedClipEffect()
    {
        if (selection_.kind != TimelineScoreSelectionKind::Clip)
            return false;
        return commitDiscreteEdit(
            bridge_.editor().removeLastScoreClipEffect(selection_.entityId), "Removed the clip's last effect");
    }

    // The selected Sound's polyphony, the notes it plays at
    // once on its track's MIDI channel; empty plays the Sound as it is.
    bool setSelectedClipPolyphony(std::optional<unsigned> polyphony)
    {
        if (selection_.kind != TimelineScoreSelectionKind::Clip)
            return false;
        auto set = talas::app::timelineWithClipPolyphony(bridge_.editor().document(), selection_.entityId, polyphony);
        if (!set.ok())
        {
            onStatus_(juceString(set.diagnostic));
            return false;
        }
        return commitDiscreteEdit(bridge_.editor().applyDocumentEdit(std::move(*set.document)),
            polyphony.has_value() ? "Set the Sound's polyphony to " + juce::String(static_cast<int>(*polyphony)) + " voices"
                                  : juce::String{"The Sound plays as it is, without a polyphony"});
    }

    void askForSelectedClipPolyphony()
    {
        if (selection_.kind != TimelineScoreSelectionKind::Clip)
            return;
        const auto current = talas::app::timelineClipPolyphony(bridge_.editor().document(), selection_.entityId);
        auto* window = new juce::AlertWindow("Polyphony",
            "How many notes the Sound plays at once, on its track's MIDI channel (empty: the Sound as it is).",
            juce::MessageBoxIconType::NoIcon, this);
        window->addTextEditor("voices", current.has_value() ? juce::String(static_cast<int>(*current)) : juce::String{}, "Voices");
        window->addButton("OK", 1, juce::KeyPress{juce::KeyPress::returnKey});
        window->addButton("Cancel", 0, juce::KeyPress{juce::KeyPress::escapeKey});
        const auto selection = selection_;
        juce::Component::SafePointer<ScoreViewportComponent> safeThis(this);
        window->enterModalState(true,
            juce::ModalCallbackFunction::create([safeThis, window, selection](int result) {
                if (safeThis == nullptr || result != 1)
                    return;
                const auto text = window->getTextEditorContents("voices").trim();
                safeThis->selection_ = selection;
                (void) safeThis->setSelectedClipPolyphony(
                    text.isEmpty() ? std::nullopt : std::optional<unsigned>{static_cast<unsigned>(std::max(0, text.getIntValue()))});
            }),
            true);
    }

    void mouseWheelMove(
        const juce::MouseEvent& event,
        const juce::MouseWheelDetails& wheel) override
    {
        if (event.mods.isCommandDown() || event.mods.isCtrlDown())
        {
            zoom_.setValue(zoom_.getValue() * (wheel.deltaY > 0.0F ? 1.25 : 0.8));
            return;
        }
        if (event.mods.isShiftDown() || std::abs(wheel.deltaX) > std::abs(wheel.deltaY))
        {
            horizontal_.setCurrentRangeStart(
                horizontal_.getCurrentRangeStart()
                    - static_cast<double>(wheel.deltaX != 0.0F
                        ? wheel.deltaX : wheel.deltaY)
                        * horizontal_.getCurrentRangeSize() * 0.2);
            return;
        }
        vertical_.setCurrentRangeStart(
            vertical_.getCurrentRangeStart()
                - static_cast<double>(wheel.deltaY)
                    * std::max(1.0, vertical_.getCurrentRangeSize() * 0.35));
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        const auto keyCode = key.getKeyCode();
        if (keyCode == juce::KeyPress::escapeKey && gestureToken_.has_value())
        {
            cancelOwnedGesture();
            return true;
        }
        if ((keyCode == juce::KeyPress::upKey || keyCode == juce::KeyPress::downKey) && key.getModifiers().isAltDown()
            && selection_.kind == TimelineScoreSelectionKind::Clip)
            return moveSelectedClipToTrack(keyCode == juce::KeyPress::downKey ? 1 : -1);
        if ((keyCode == juce::KeyPress::leftKey
                || keyCode == juce::KeyPress::rightKey)
            && key.getModifiers().isAltDown())
        {
            return nudgeSelection(
                keyCode == juce::KeyPress::rightKey ? 1 : -1,
                key.getModifiers().isShiftDown());
        }
        if (keyCode == juce::KeyPress::deleteKey
            || keyCode == juce::KeyPress::backspaceKey)
        {
            return deleteSelectedPoints() || deleteSelectedClips() || deleteSelection();
        }
        // The arrows nudge the selected points,
        // Shift by more.
        if ((keyCode == juce::KeyPress::leftKey || keyCode == juce::KeyPress::rightKey)
            && selection_.kind == TimelineScoreSelectionKind::AutomationPoint
            && !key.getModifiers().isCommandDown() && !key.getModifiers().isCtrlDown())
        {
            return nudgeSelection(keyCode == juce::KeyPress::rightKey ? 1 : -1, key.getModifiers().isShiftDown());
        }
        // Enter, to type a value for the selected points.
        if (keyCode == juce::KeyPress::returnKey && selection_.kind == TimelineScoreSelectionKind::AutomationPoint)
        {
            askSelectedPointsValue();
            return true;
        }
        if (key.getTextCharacter() == '-' && selection_.kind == TimelineScoreSelectionKind::Clip)
            return removeSelectedClipEffect();
        const auto textCharacter = key.getTextCharacter();
        const auto shortcutCharacter = textCharacter != 0
            ? textCharacter
            : keyCode >= 0 && keyCode <= 0xff
            ? static_cast<juce::juce_wchar>(keyCode)
            : juce::juce_wchar{};
        const auto character = static_cast<char>(
            std::tolower(static_cast<unsigned char>(shortcutCharacter)));
        if (character == 'd' && key.getModifiers().isCommandDown())
            return duplicateSelectedClip();
        // Ctrl+C copies the selected Sounds,
        // Ctrl+V pastes them on the track clicked last, at the time clicked.
        if (character == 'c' && key.getModifiers().isCommandDown() && selection_.kind == TimelineScoreSelectionKind::Clip)
            return copySelectedClips();
        if (character == 'v' && key.getModifiers().isCommandDown() && (!clipsCopied_.clips.empty() || soundToPaste_))
            return pasteCopiedClips();
        // Ctrl+A (Command on the Mac) selects all of a lane's points.
        if (character == 'a' && key.getModifiers().isCommandDown() && selectAllPointsOfLane())
            return true;
        const auto globalCommandModifier = key.getModifiers().isCommandDown()
            || key.getModifiers().isCtrlDown();
        if (globalCommandModifier)
            return juce::Component::keyPressed(key);
        if (character == 'k')
            return createMarkerAtFrame(
                runtime_.playheadFrame,
                key.getModifiers().isShiftDown());
        if (character == 'p')
            return addAutomationPointAtFrame(
                runtime_.playheadFrame,
                key.getModifiers().isShiftDown());
        if (character == 'c')
        {
            contextPointerFrame_ = runtime_.playheadFrame;
            showContextMenu();
            return true;
        }
        if (character == 'e')
            return toggleControllerRecording();
        if (character == 'v')
            return previewControllerRecording();
        if (keyCode == juce::KeyPress::returnKey)
            return approveControllerRecording();
        if (shortcutCharacter == '[')
            return resizeSelectedClipEnd(-1);
        if (shortcutCharacter == ']')
            return resizeSelectedClipEnd(1);
        if (shortcutCharacter == ',')
            return trimSelectedClipStart(-1);
        if (shortcutCharacter == '.')
            return trimSelectedClipStart(1);
        if (character == 'g')
            return seekToSelection();
        if (character == 'n')
            return addScoreTrack();
        if (character == 'a')
            return addScoreAutomationLane();
        if (character == 'r')
            return adjustSelectedClipRepeat(
                key.getModifiers().isShiftDown() ? -1 : 1);
        if (character == 'u')
            return toggleSelectedTrackMute();
        if (character == 's')
            return toggleSelectedTrackSolo();
        if (character == 'l')
            return toggleSelectedAutomationInterpolation();
        if (keyCode == juce::KeyPress::leftKey
            || keyCode == juce::KeyPress::rightKey
            || keyCode == juce::KeyPress::upKey
            || keyCode == juce::KeyPress::downKey)
            return handleAccessibleKey(key, selectedFocusIdentity_);
        if (key.getTextCharacter() == '+' || key.getTextCharacter() == '=')
        {
            zoom_.setValue(zoom_.getValue() * 1.25);
            return true;
        }
        if (key.getTextCharacter() == '-')
        {
            zoom_.setValue(zoom_.getValue() * 0.8);
            return true;
        }
        return juce::Component::keyPressed(key);
    }

    bool keyStateChanged(bool) override
    {
        if (!keyboardGesture_ || !gestureToken_.has_value())
            return false;
        if (juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::leftKey)
            || juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::rightKey))
        {
            return false;
        }
        finishOwnedGesture();
        return true;
    }

private:
    [[nodiscard]] std::optional<std::uint64_t> activeFrameFor(
        const talas::project::ScoreTime& time) const noexcept
    {
        return talas::project::scoreTimeToSampleFrame(
            time,
            bridge_.editor().document().timeline.musicalTime,
            runtime_.sampleRate);
    }

    [[nodiscard]] bool clipProjectsAtActiveRate(
        const talas::project::ScoreClip& clip) const noexcept
    {
        const auto repeated = multiplyScoreTime(
            clip.duration, clip.repeatCount);
        const auto endTime = repeated.has_value()
            ? addScoreTime(clip.start, *repeated)
            : std::nullopt;
        const auto begin = activeFrameFor(clip.start);
        const auto end = endTime.has_value()
            ? activeFrameFor(*endTime)
            : std::nullopt;
        return begin.has_value() && end.has_value() && *end > *begin;
    }

    [[nodiscard]] bool markerProjectsAtActiveRate(
        const talas::project::ScoreMarker& marker) const noexcept
    {
        const auto begin = activeFrameFor(marker.start);
        if (!begin.has_value()
            || *begin == std::numeric_limits<std::uint64_t>::max())
            return false;
        if (!marker.end.has_value())
            return true;
        const auto end = activeFrameFor(*marker.end);
        return end.has_value() && *end > *begin;
    }

    [[nodiscard]] bool laneProjectsAtActiveRate(
        const talas::project::ScoreAutomationLane& lane) const noexcept
    {
        const auto& score = bridge_.editor().document().timeline.score;
        const auto owner = lane.scope == talas::project::ScoreAutomationScope::Clip
            ? std::ranges::find(
                score.clips, lane.ownerId,
                &talas::project::ScoreClip::clipId)
            : score.clips.end();
        for (const auto& point : lane.points)
        {
            const auto absolute = owner != score.clips.end()
                ? addScoreTime(owner->start, point.position)
                : std::optional<talas::project::ScoreTime>{point.position};
            const auto frame = absolute.has_value()
                ? activeFrameFor(*absolute)
                : std::nullopt;
            if (!frame.has_value()
                || *frame == std::numeric_limits<std::uint64_t>::max())
                return false;
        }
        return true;
    }

    [[nodiscard]] talas::project::ScoreTimeDomain selectedTimeDomain() const
    {
        const auto& score = bridge_.editor().document().timeline.score;
        if (selection_.kind == TimelineScoreSelectionKind::Clip)
        {
            const auto clip = std::ranges::find(
                score.clips, selection_.entityId,
                &talas::project::ScoreClip::clipId);
            if (clip != score.clips.end())
                return clip->start.domain;
        }
        if (selection_.kind == TimelineScoreSelectionKind::Marker)
        {
            const auto marker = std::ranges::find(
                score.markers, selection_.entityId,
                &talas::project::ScoreMarker::markerId);
            if (marker != score.markers.end())
                return marker->start.domain;
        }
        if (selection_.kind == TimelineScoreSelectionKind::AutomationPoint
            || selection_.kind == TimelineScoreSelectionKind::AutomationLane)
        {
            const auto lane = std::ranges::find(
                score.automation, selection_.entityId,
                &talas::project::ScoreAutomationLane::laneId);
            if (lane != score.automation.end() && !lane->points.empty())
                return lane->points.front().position.domain;
        }
        if (selection_.kind == TimelineScoreSelectionKind::Track)
        {
            const auto track = std::ranges::find(
                score.tracks, selection_.entityId,
                &talas::project::ScoreTrack::trackId);
            if (track != score.tracks.end())
                return track->timeDomain;
        }
        return talas::project::ScoreTimeDomain::Seconds;
    }

    [[nodiscard]] int scoreToolbarHeight() const noexcept
    {
        return std::max(26, static_cast<int>(std::lround(
            26.0F * WorkbenchType::interfaceScale())));
    }
    // One of the two strips across the top: the markers', then the time
    // ruler's.
    [[nodiscard]] int scoreStripHeight() const noexcept
    {
        return std::max(24, static_cast<int>(std::lround(
            24.0F * WorkbenchType::interfaceScale())));
    }
    // Both strips, above the tracks' rows.
    [[nodiscard]] int scoreRulerHeight() const noexcept
    {
        return 2 * scoreStripHeight();
    }
    [[nodiscard]] int scoreScrollBarWidth() const noexcept
    {
        return std::max(16, static_cast<int>(std::lround(
            16.0F * WorkbenchType::interfaceScale())));
    }
    [[nodiscard]] int scoreRowHeight() const noexcept
    {
        return std::max(30, static_cast<int>(std::lround(
            34.0F * WorkbenchType::interfaceScale())));
    }
    [[nodiscard]] int scoreHeaderWidth() const noexcept
    {
        const auto base = getWidth() < 560 ? 112.0F : 148.0F;
        return std::min(
            std::max(1, getWidth() / 2),
            static_cast<int>(std::lround(
                base * WorkbenchType::interfaceScale())));
    }
    [[nodiscard]] juce::Rectangle<int> contentBounds() const
    {
        auto bounds = getLocalBounds();
        bounds.removeFromTop(2 * scoreToolbarHeight());
        bounds.removeFromRight(scoreScrollBarWidth());
        bounds.removeFromBottom(scoreScrollBarWidth());
        return bounds;
    }
    [[nodiscard]] TimelineRulerMode rulerMode() const noexcept
    {
        switch (rulerMode_.getSelectedId())
        {
        case 1: return TimelineRulerMode::Seconds;
        case 3: return TimelineRulerMode::Smpte;
        case 4: return TimelineRulerMode::MinutesSeconds;
        default: return TimelineRulerMode::BarsBeatsTicks;
        }
    }

    [[nodiscard]] std::optional<std::uint64_t> frameAtPointerX(
        float componentX) const noexcept
    {
        if (viewport_.state != TimelineScoreSurfaceState::Content
            || (viewport_.rows.empty() && viewport_.items.empty()))
        {
            return std::nullopt;
        }
        const auto rows = contentBounds().withTrimmedTop(scoreRulerHeight());
        const auto left = rows.getX() + scoreHeaderWidth();
        const auto width = rows.getRight() - left;
        if (width <= 0 || viewport_.rulerTicks.empty())
            return std::nullopt;
        return talas::app::timelineScoreFrameAtPointer(
            static_cast<double>(componentX - static_cast<float>(left)), static_cast<double>(width), viewportStartFrame_,
            viewportEndFrame_);
    }

    [[nodiscard]] std::uint64_t shiftedFrame(
        std::uint64_t base,
        std::uint64_t origin,
        std::uint64_t current) const noexcept
    {
        return talas::app::timelineScoreDraggedFrame(base, origin, current);
    }

    [[nodiscard]] std::optional<talas::project::ScoreTime> timeAtFrame(
        std::uint64_t frame,
        talas::project::ScoreTimeDomain domain,
        bool acceptSnap) const noexcept
    {
        const auto pointer = timelineScorePointerTime(
            frame,
            domain,
            bridge_.editor().document().timeline.musicalTime,
            runtime_.sampleRate);
        if (!pointer.has_value())
            return std::nullopt;
        auto chosen = acceptSnap && pointer->snapCandidate.has_value()
            ? pointer->snapCandidate
            : std::optional<talas::project::ScoreTime>{pointer->exact};
        // A Quantized grid places on the quantum.
        if (gridModeValue_ == talas::app::TimelineGridMode::Quantized)
            chosen = talas::app::timelineQuantized(pointer->exact, nudgeAmount(domain, false));
        const auto roundTrip = chosen.has_value()
            ? talas::project::scoreTimeToSampleFrame(
                *chosen,
                bridge_.editor().document().timeline.musicalTime,
                runtime_.sampleRate)
            : std::nullopt;
        return roundTrip.has_value()
                && *roundTrip != std::numeric_limits<std::uint64_t>::max()
            ? chosen
            : std::nullopt;
    }

    void preparePointerGesture(const juce::MouseEvent& event)
    {
        gestureMode_ = GestureMode::None;
        gestureClip_.reset();
        gestureMarker_.reset();
        gestureLane_.reset();
        gestureChanged_ = false;
        keyboardGesture_ = false;
        gestureStartPosition_ = event.position;
        gestureAxis_.reset();
        const auto pointerFrame = frameAtPointerX(event.position.x);
        if (!pointerFrame.has_value())
            return;
        const auto item = std::ranges::find_if(
            viewport_.items,
            [this](const auto& candidate) {
                return candidate.focusIdentity == selectedFocusIdentity_;
            });
        if (item == viewport_.items.end())
            return;
        gesturePointerFrame_ = *pointerFrame;
        gestureEntityFrame_ = item->startFrame;
        const auto& score = bridge_.editor().document().timeline.score;
        if (selection_.kind == TimelineScoreSelectionKind::Clip)
        {
            const auto clip = std::ranges::find(
                score.clips, selection_.entityId, &talas::project::ScoreClip::clipId);
            if (clip == score.clips.end())
                return;
            gestureClip_ = *clip;
            const auto rows = contentBounds().withTrimmedTop(scoreRulerHeight());
            const auto itemLeft = static_cast<float>(
                rows.getX() + scoreHeaderWidth()) + static_cast<float>(item->x);
            const auto itemRight = itemLeft + static_cast<float>(item->width);
            // The right edge drags the Sound's end.
            switch (talas::app::timelineClipGesture(event.position.x, itemLeft, itemRight))
            {
            case talas::app::TimelineClipGesture::TrimStart: gestureMode_ = GestureMode::TrimStart; break;
            case talas::app::TimelineClipGesture::ResizeEnd: gestureMode_ = GestureMode::ResizeEnd; break;
            case talas::app::TimelineClipGesture::Move: gestureMode_ = GestureMode::Move; break;
            }
            if (gestureMode_ == GestureMode::ResizeEnd)
                gestureEntityFrame_ = item->endFrame;
            return;
        }
        if (selection_.kind == TimelineScoreSelectionKind::Marker)
        {
            const auto marker = std::ranges::find(
                score.markers, selection_.entityId,
                &talas::project::ScoreMarker::markerId);
            if (marker != score.markers.end())
            {
                gestureMarker_ = *marker;
                gestureMode_ = GestureMode::Move;
            }
            return;
        }
        if (selection_.kind == TimelineScoreSelectionKind::AutomationPoint)
        {
            const auto lane = std::ranges::find(
                score.automation, selection_.entityId,
                &talas::project::ScoreAutomationLane::laneId);
            if (lane != score.automation.end()
                && selection_.pointIndex < lane->points.size())
            {
                gestureLane_ = *lane;
                gestureMode_ = GestureMode::Move;
            }
        }
    }

    void applyPointerGesture(std::uint64_t targetFrame, bool acceptSnap,
        std::optional<double> heightMoved = std::nullopt, bool moveTime = true)
    {
        if (targetFrame == std::numeric_limits<std::uint64_t>::max())
        {
            onStatus_("Score gesture rejected outside the half-open Timeline range");
            return;
        }
        talas::project::EditResult result;
        if (gestureClip_.has_value())
        {
            auto clip = *gestureClip_;
            if (gestureMode_ == GestureMode::Move)
            {
                const auto target = timeAtFrame(
                    targetFrame, clip.start.domain, acceptSnap);
                if (!target.has_value())
                    return;
                clip.start = *target;
            }
            else
            {
                const auto repeatedDuration = multiplyScoreTime(
                    clip.duration, clip.repeatCount);
                const auto originalEnd = repeatedDuration.has_value()
                    ? addScoreTime(clip.start, *repeatedDuration)
                    : std::nullopt;
                const auto target = timeAtFrame(
                    targetFrame, clip.start.domain, acceptSnap);
                if (!originalEnd.has_value() || !target.has_value())
                    return;
                if (gestureMode_ == GestureMode::ResizeEnd)
                {
                    const auto totalDuration = subtractScoreTime(*target, clip.start);
                    const auto duration = totalDuration.has_value()
                        ? divideScoreTime(*totalDuration, clip.repeatCount)
                        : std::nullopt;
                    if (!duration.has_value() || !positiveScoreTime(*duration))
                        return;
                    clip.duration = *duration;
                }
                else
                {
                    const auto totalDuration = subtractScoreTime(
                        *originalEnd, *target);
                    const auto duration = totalDuration.has_value()
                        ? divideScoreTime(*totalDuration, clip.repeatCount)
                        : std::nullopt;
                    auto offset = std::optional<talas::project::ScoreTime>{};
                    if (const auto trim = subtractScoreTime(*target, clip.start);
                        trim.has_value())
                    {
                        offset = addScoreTime(clip.contentOffset, *trim);
                    }
                    else if (const auto extension = subtractScoreTime(
                                 clip.start, *target);
                             extension.has_value())
                    {
                        offset = subtractScoreTime(
                            clip.contentOffset, *extension);
                    }
                    if (!duration.has_value() || !positiveScoreTime(*duration)
                        || !offset.has_value())
                    {
                        return;
                    }
                    clip.start = *target;
                    clip.duration = *duration;
                    clip.contentOffset = *offset;
                }
                const auto totalDuration = multiplyScoreTime(
                    clip.duration, clip.repeatCount);
                if (!totalDuration.has_value())
                    return;
                if (!clampClipFadesToTotalDuration(clip, *totalDuration))
                    return;
            }
            if (!clipProjectsAtActiveRate(clip))
            {
                onStatus_("Score clip edit exceeds the active Timeline sample range");
                return;
            }
            result = bridge_.editor().updateScoreClip(
                std::move(clip), gestureToken_.value_or(0));
        }
        else if (gestureMarker_.has_value())
        {
            auto marker = *gestureMarker_;
            const auto target = timeAtFrame(
                targetFrame, marker.start.domain, acceptSnap);
            if (!target.has_value())
                return;
            if (marker.end.has_value())
            {
                const auto duration = subtractScoreTime(*marker.end, marker.start);
                const auto movedEnd = duration.has_value()
                    ? addScoreTime(*target, *duration)
                    : std::nullopt;
                if (!movedEnd.has_value())
                    return;
                marker.end = *movedEnd;
            }
            marker.start = *target;
            if (!markerProjectsAtActiveRate(marker))
            {
                onStatus_("Score marker edit exceeds the active Timeline sample range");
                return;
            }
            result = bridge_.editor().updateScoreMarker(
                std::move(marker), gestureToken_.value_or(0));
        }
        else if (gestureLane_.has_value())
        {
            auto lane = *gestureLane_;
            auto target = moveTime
                ? timeAtFrame(
                    targetFrame,
                    lane.points[selection_.pointIndex].position.domain,
                    acceptSnap)
                : std::optional<talas::project::ScoreTime>{lane.points[selection_.pointIndex].position};
            if (!target.has_value())
                return;
            if (moveTime && lane.scope == talas::project::ScoreAutomationScope::Clip)
            {
                const auto& clips = bridge_.editor().document().timeline.score.clips;
                const auto owner = std::ranges::find(
                    clips, lane.ownerId, &talas::project::ScoreClip::clipId);
                target = owner != clips.end()
                    ? subtractScoreTime(*target, owner->start)
                    : std::nullopt;
                if (!target.has_value())
                    return;
            }
            lane.points[selection_.pointIndex].position = *target;
            if (heightMoved.has_value())
            {
                const auto range = talas::app::automationValueRange(gestureLane_->points, laneValueZoom(gestureLane_->laneId));
                lane.points[selection_.pointIndex].value = laneValueZoom(gestureLane_->laneId) == 0
                    ? talas::app::automationValueDragged(*gestureLane_, gestureLane_->points[selection_.pointIndex].value, *heightMoved)
                    : gestureLane_->points[selection_.pointIndex].value + *heightMoved * (range.high - range.low);
            }
            if (!laneProjectsAtActiveRate(lane))
            {
                onStatus_("Score automation edit exceeds the active Timeline sample range");
                return;
            }
            result = bridge_.editor().updateScoreAutomationLane(
                std::move(lane), gestureToken_.value_or(0));
            if (result.ok())
                selectedPointPosition_ = *target;
        }
        else
        {
            return;
        }

        if (!result.ok())
        {
            onStatus_(juceString(result.message));
            return;
        }
        gestureChanged_ = true;
        rebuild(bridge_.editor().document(), runtime_, true);
        onStatus_(acceptSnap
            ? juceString("Score gesture · snap candidate accepted")
            : juceString("Score gesture · exact"));
    }

    void finishOwnedGesture()
    {
        if (gestureToken_.has_value())
            bridge_.editor().endOwnedHistoryTransaction(*gestureToken_);
        const auto changed = gestureChanged_;
        gestureToken_.reset();
        gestureMode_ = GestureMode::None;
        gestureClip_.reset();
        gestureMarker_.reset();
        gestureLane_.reset();
        gestureChanged_ = false;
        keyboardGesture_ = false;
        if (changed)
        {
            onDocumentChange_();
            onStatus_("Committed one Score gesture");
        }
    }

    void cancelOwnedGesture(bool announce = true)
    {
        const auto hadGesture = gestureToken_.has_value();
        if (gestureToken_.has_value())
            bridge_.editor().cancelOwnedHistoryTransaction(*gestureToken_);
        gestureToken_.reset();
        gestureMode_ = GestureMode::None;
        gestureClip_.reset();
        gestureMarker_.reset();
        gestureLane_.reset();
        gestureChanged_ = false;
        keyboardGesture_ = false;
        if (hadGesture && projection_ != nullptr)
            rebuild(bridge_.editor().document(), runtime_);
        if (hadGesture && announce)
            onStatus_("Cancelled Score gesture");
    }

    void revokeOwnedGestureForExternalRebuild()
    {
        if (!gestureToken_.has_value())
            return;
        bridge_.editor().cancelOwnedHistoryTransaction(*gestureToken_);
        gestureToken_.reset();
        gestureMode_ = GestureMode::None;
        gestureClip_.reset();
        gestureMarker_.reset();
        gestureLane_.reset();
        gestureChanged_ = false;
        keyboardGesture_ = false;
        onStatus_("Cancelled stale Score gesture after Timeline authority changed");
    }

    // One quantum, or with Shift the smallest unit
    // of time, a frame, a beat or a second.
    [[nodiscard]] talas::project::ScoreTime nudgeAmount(
        talas::project::ScoreTimeDomain domain,
        bool coarse) const
    {
        const auto& musical = bridge_.editor().document().timeline.musicalTime;
        return coarse ? talas::app::timelineTimeUnit(rulerMode(), domain, musical, framesPerSecond())
                      : talas::app::timelineQuantum(quantizationValue_, rulerMode(), domain, musical, framesPerSecond());
    }

    [[nodiscard]] double framesPerSecond() const noexcept
    {
        switch (runtime_.smpteRate)
        {
        case TimelineSmpteRate::Fps24: return 24.0;
        case TimelineSmpteRate::Fps25: return 25.0;
        case TimelineSmpteRate::Fps2997Drop: return 30000.0 / 1001.0;
        case TimelineSmpteRate::Fps30: return 30.0;
        }
        return 30.0;
    }

public:
    bool setGridMode(talas::app::TimelineGridMode mode)
    {
        gridModeValue_ = mode;
        gridMode_.setSelectedId(static_cast<int>(mode) + 1, juce::dontSendNotification);
        onStatus_(juceString(std::string{talas::app::timelineGridModeName(mode)}));
        return true;
    }

    bool setQuantization(talas::app::TimelineQuantization quantization)
    {
        quantizationValue_ = quantization;
        refreshQuantizationMenu();
        onStatus_("Quantization " + juceString(talas::app::timelineQuantizationText(quantization)));
        return true;
    }

    [[nodiscard]] talas::project::ScoreTime nudgeAmountForSmoke(talas::project::ScoreTimeDomain domain, bool coarse) const
    {
        return nudgeAmount(domain, coarse);
    }

    // The up and down arrows (Option with them here) move the
    // selected Sound a track, over the occupied ones.
    bool moveSelectedClipToTrack(int direction)
    {
        if (selection_.kind != TimelineScoreSelectionKind::Clip)
            return false;
        const auto& score = bridge_.editor().document().timeline.score;
        const auto clip = std::ranges::find(score.clips, selection_.entityId, &talas::project::ScoreClip::clipId);
        if (clip == score.clips.end())
            return false;
        const auto target = talas::app::timelineTrackForMove(score, clip->clipId, direction);
        if (!target.has_value())
        {
            onStatus_("No track that way has room for the Sound");
            return true;
        }
        auto moved = *clip;
        moved.trackId = *target;
        return commitDiscreteEdit(bridge_.editor().updateScoreClip(std::move(moved)), "Moved the Sound to " + juceString(*target));
    }

private:
    static constexpr int quantizationEnterId = 100;

    void refreshQuantizationMenu()
    {
        quantization_.clear(juce::dontSendNotification);
        const auto choices = talas::app::timelineQuantizationChoices();
        for (std::size_t index = 0; index < choices.size(); ++index)
            quantization_.addItem(juceString(talas::app::timelineQuantizationText(choices[index])), static_cast<int>(index) + 1);
        const auto chosen = std::ranges::find(choices, quantizationValue_);
        if (chosen == choices.end())
            quantization_.addItem(juceString(talas::app::timelineQuantizationText(quantizationValue_)), quantizationEnterId + 1);
        quantization_.addItem("Enter...", quantizationEnterId);
        quantization_.setSelectedId(chosen == choices.end() ? quantizationEnterId + 1
                                                            : static_cast<int>(chosen - choices.begin()) + 1,
            juce::dontSendNotification);
    }

    void askForQuantization()
    {
        auto* window = new juce::AlertWindow("Quantization",
            "Type a fraction of a frame, a beat or a second, such as 1/24.", juce::MessageBoxIconType::NoIcon, this);
        window->addTextEditor("fraction", juceString(talas::app::timelineQuantizationText(quantizationValue_)));
        window->addButton("OK", 1, juce::KeyPress{juce::KeyPress::returnKey});
        window->addButton("Cancel", 0, juce::KeyPress{juce::KeyPress::escapeKey});
        juce::Component::SafePointer<ScoreViewportComponent> safeThis(this);
        window->enterModalState(true,
            juce::ModalCallbackFunction::create([safeThis, window](int result) {
                if (safeThis == nullptr)
                    return;
                const auto parsed = result == 1
                    ? talas::app::parseTimelineQuantization(window->getTextEditorContents("fraction").toStdString())
                    : std::nullopt;
                if (parsed.has_value())
                    (void) safeThis->setQuantization(*parsed);
                else
                    safeThis->refreshQuantizationMenu();
            }),
            true);
    }

    [[nodiscard]] std::optional<talas::project::ScoreTime> nudgeTime(
        const talas::project::ScoreTime& value,
        int direction,
        bool coarse) const noexcept
    {
        const auto amount = nudgeAmount(value.domain, coarse);
        return direction > 0
            ? addScoreTime(value, amount)
            : subtractScoreTime(value, amount);
    }

    bool nudgeSelection(int direction, bool coarse)
    {
        const auto& score = bridge_.editor().document().timeline.score;
        talas::project::EditResult result;
        const auto selectionExists = [&]() {
            if (selection_.kind == TimelineScoreSelectionKind::Clip)
            {
                return std::ranges::any_of(score.clips, [this](const auto& clip) {
                    return clip.clipId == selection_.entityId;
                });
            }
            if (selection_.kind == TimelineScoreSelectionKind::Marker)
            {
                return std::ranges::any_of(score.markers, [this](const auto& marker) {
                    return marker.markerId == selection_.entityId;
                });
            }
            if (selection_.kind == TimelineScoreSelectionKind::AutomationPoint)
            {
                const auto lane = std::ranges::find(
                    score.automation, selection_.entityId,
                    &talas::project::ScoreAutomationLane::laneId);
                return lane != score.automation.end()
                    && selection_.pointIndex < lane->points.size();
            }
            return false;
        }();
        if (!selectionExists)
            return false;
        if (!gestureToken_.has_value())
        {
            gestureToken_ = bridge_.editor().beginOwnedHistoryTransaction();
            keyboardGesture_ = true;
        }
        if (selection_.kind == TimelineScoreSelectionKind::Clip)
        {
            const auto found = std::ranges::find(
                score.clips, selection_.entityId, &talas::project::ScoreClip::clipId);
            if (found == score.clips.end())
                return false;
            auto clip = *found;
            const auto moved = nudgeTime(clip.start, direction, coarse);
            if (!moved.has_value())
                return true;
            // A selected group of Sounds nudges together.
            if (const auto ids = selectedClipIds(); ids.size() > 1)
            {
                const auto first = talas::app::earliestClip(score, ids);
                const auto earliest = std::ranges::find(score.clips, *first, &talas::project::ScoreClip::clipId);
                const auto start = nudgeTime(earliest->start, direction, coarse);
                const auto group = start.has_value() ? talas::app::clipsMovedTogether(score, ids, *start) : std::nullopt;
                if (group.has_value())
                    (void) commitClipGroup(*group, "Nudged the selected Sounds");
                return true;
            }
            clip.start = *moved;
            if (!clipProjectsAtActiveRate(clip))
                return true;
            result = bridge_.editor().updateScoreClip(
                std::move(clip), gestureToken_.value_or(0));
        }
        else if (selection_.kind == TimelineScoreSelectionKind::Marker)
        {
            const auto found = std::ranges::find(
                score.markers, selection_.entityId,
                &talas::project::ScoreMarker::markerId);
            if (found == score.markers.end())
                return false;
            auto marker = *found;
            const auto moved = nudgeTime(marker.start, direction, coarse);
            const auto movedEnd = marker.end.has_value()
                ? nudgeTime(*marker.end, direction, coarse)
                : std::optional<talas::project::ScoreTime>{};
            if (!moved.has_value() || (marker.end.has_value() && !movedEnd.has_value()))
                return true;
            marker.start = *moved;
            if (marker.end.has_value())
                marker.end = *movedEnd;
            if (!markerProjectsAtActiveRate(marker))
                return true;
            result = bridge_.editor().updateScoreMarker(
                std::move(marker), gestureToken_.value_or(0));
        }
        else if (selection_.kind == TimelineScoreSelectionKind::AutomationPoint)
        {
            const auto found = std::ranges::find(
                score.automation, selection_.entityId,
                &talas::project::ScoreAutomationLane::laneId);
            if (found == score.automation.end()
                || selection_.pointIndex >= found->points.size())
            {
                return false;
            }
            auto lane = *found;
            const auto moved = nudgeTime(
                lane.points[selection_.pointIndex].position, direction, coarse);
            if (!moved.has_value())
                return true;
            if (const auto indices = selectedPointIndices(); indices.size() > 1)
            {
                const auto first = nudgeTime(lane.points[indices.front()].position, direction, coarse);
                if (first.has_value())
                    (void) moveSelectedPointsTo(talas::app::automationPointTime(*first));
                gestureChanged_ = true;
                return true;
            }
            lane.points[selection_.pointIndex].position = *moved;
            if (!laneProjectsAtActiveRate(lane))
                return true;
            result = bridge_.editor().updateScoreAutomationLane(
                std::move(lane), gestureToken_.value_or(0));
            if (result.ok())
                selectedPointPosition_ = *moved;
        }
        else
        {
            return false;
        }
        if (!result.ok())
        {
            onStatus_(juceString(result.message));
            return true;
        }
        gestureChanged_ = true;
        rebuild(bridge_.editor().document(), runtime_, true);
        onStatus_(coarse
            ? juceString("Nudged Score selection · coarse")
            : juceString("Nudged Score selection · fine"));
        return true;
    }

    bool commitDiscreteEdit(
        const talas::project::EditResult& result,
        juce::String successStatus)
    {
        if (!result.ok())
        {
            onStatus_(juceString(result.message));
            return true;
        }
        onDocumentChange_();
        rebuild(bridge_.editor().document(), runtime_);
        onStatus_(std::move(successStatus));
        return true;
    }

    bool deleteSelection()
    {
        finishOwnedGesture();
        if (!timelineScoreHasCommand(
                viewport_.commands, TimelineScoreCommand::DeleteSelection))
        {
            onStatus_("Delete is unavailable for the exact Score selection");
            return selection_.kind != TimelineScoreSelectionKind::None;
        }
        switch (selection_.kind)
        {
        case TimelineScoreSelectionKind::Track:
            return commitDiscreteEdit(
                bridge_.editor().removeScoreTrack(selection_.entityId),
                "Deleted Score track");
        case TimelineScoreSelectionKind::Clip:
            return commitDiscreteEdit(
                bridge_.editor().removeScoreClip(selection_.entityId),
                "Deleted Score clip");
        case TimelineScoreSelectionKind::AutomationLane:
            return commitDiscreteEdit(
                bridge_.editor().removeScoreAutomationLane(selection_.entityId),
                "Deleted Score automation lane");
        case TimelineScoreSelectionKind::AutomationPoint:
        {
            const auto& lanes = bridge_.editor().document().timeline.score.automation;
            const auto found = std::ranges::find(
                lanes, selection_.entityId, &talas::project::ScoreAutomationLane::laneId);
            if (found == lanes.end() || selection_.pointIndex >= found->points.size())
                return false;
            auto lane = *found;
            lane.points.erase(lane.points.begin()
                + static_cast<std::ptrdiff_t>(selection_.pointIndex));
            const auto result = bridge_.editor().updateScoreAutomationLane(
                std::move(lane));
            if (result.ok())
            {
                selection_ = {
                    TimelineScoreSelectionKind::AutomationLane,
                    selection_.entityId,
                    0,
                };
                selectedFocusIdentity_ = "timeline.automation:"
                    + selection_.entityId;
                selectedPointPosition_.reset();
            }
            return commitDiscreteEdit(result, "Deleted Score automation point");
        }
        case TimelineScoreSelectionKind::Marker:
            return commitDiscreteEdit(
                bridge_.editor().removeScoreMarker(selection_.entityId),
                "Deleted Score marker");
        case TimelineScoreSelectionKind::None:
            break;
        }
        return false;
    }

    [[nodiscard]] std::string uniqueScoreId(std::string_view prefix) const
    {
        const auto& score = bridge_.editor().document().timeline.score;
        const auto used = [&score](std::string_view id) {
            return std::ranges::any_of(score.tracks, [id](const auto& item) {
                       return item.trackId == id;
                   })
                || std::ranges::any_of(score.clips, [id](const auto& item) {
                       return item.clipId == id;
                   })
                || std::ranges::any_of(score.automation, [id](const auto& item) {
                       return item.laneId == id;
                   })
                || std::ranges::any_of(score.markers, [id](const auto& item) {
                       return item.markerId == id;
                   });
        };
        for (std::uint32_t suffix = 1; suffix < 1'000'000U; ++suffix)
        {
            auto candidate = std::string(prefix) + std::to_string(suffix);
            if (!used(candidate))
                return candidate;
        }
        return {};
    }

    bool duplicateSelectedClip()
    {
        if (!timelineScoreHasCommand(
                viewport_.commands, TimelineScoreCommand::DuplicateSelection))
            return false;
        if (selection_.kind != TimelineScoreSelectionKind::Clip)
            return false;
        const auto& clips = bridge_.editor().document().timeline.score.clips;
        const auto found = std::ranges::find(
            clips, selection_.entityId, &talas::project::ScoreClip::clipId);
        if (found == clips.end())
            return false;
        auto clip = *found;
        clip.clipId = uniqueScoreId("clip");
        const auto totalDuration = multiplyScoreTime(
            clip.duration, clip.repeatCount);
        const auto nextStart = totalDuration.has_value()
            ? addScoreTime(clip.start, *totalDuration)
            : std::nullopt;
        if (clip.clipId.empty() || !nextStart.has_value())
            return true;
        clip.start = *nextStart;
        if (!clipProjectsAtActiveRate(clip))
        {
            onStatus_("Clip duplicate exceeds the active Timeline sample range");
            return true;
        }
        return commitDiscreteEdit(
            bridge_.editor().addScoreClip(std::move(clip)),
            "Created Score clip duplicate");
    }

    bool addScoreTrack()
    {
        if (!timelineScoreHasCommand(
                viewport_.commands, TimelineScoreCommand::AddTrack))
            return false;
        const auto& score = bridge_.editor().document().timeline.score;
        const auto trackId = uniqueScoreId("track");
        if (trackId.empty())
            return true;
        auto masterId = std::string{};
        const auto master = std::ranges::find(
            score.tracks,
            talas::project::ScoreTrackKind::Master,
            &talas::project::ScoreTrack::kind);
        talas::project::ProjectScoreEditBatch edits;
        if (master == score.tracks.end())
        {
            masterId = uniqueScoreId("master");
            if (masterId.empty())
                return true;
            edits.tracks.push_back({
                masterId,
                "Master",
                talas::project::ScoreTrackKind::Master,
                talas::project::ScoreTimeDomain::Seconds,
                {},
                2,
                talas::project::ScoreOverlapMode::Layer,
            });
        }
        else
        {
            masterId = master->trackId;
        }
        edits.tracks.push_back({
            trackId,
            "Score Track",
            talas::project::ScoreTrackKind::Track,
            talas::project::ScoreTimeDomain::Seconds,
            masterId,
            2,
            talas::project::ScoreOverlapMode::Layer,
        });
        const auto result = bridge_.editor().applyScoreEdits(std::move(edits));
        if (result.ok())
        {
            selection_ = {TimelineScoreSelectionKind::Track, trackId, 0};
            selectedFocusIdentity_ = "timeline.track:" + trackId;
        }
        return commitDiscreteEdit(result, "Created explicit Score track");
    }

    bool addScoreAutomationLane()
    {
        if (!timelineScoreHasCommand(
                viewport_.commands, TimelineScoreCommand::AddAutomation)
            || !canAddGainAutomation())
            return false;
        const auto& score = bridge_.editor().document().timeline.score;
        const talas::project::ScoreTrack* owner = nullptr;
        if (selection_.kind == TimelineScoreSelectionKind::Track)
        {
            const auto found = std::ranges::find(
                score.tracks, selection_.entityId,
                &talas::project::ScoreTrack::trackId);
            if (found != score.tracks.end())
                owner = &*found;
        }
        const auto selectedOwner = owner != nullptr;
        if (selectedOwner && hasGainAutomationForOwner(*owner))
        {
            onStatus_("The selected Score track already owns exact Gain automation");
            return true;
        }
        if (!selectedOwner && owner == nullptr && !score.tracks.empty())
        {
            const auto found = std::ranges::find_if(score.tracks, [this](const auto& track) {
                return !hasGainAutomationForOwner(track);
            });
            owner = found != score.tracks.end() ? &*found : &score.tracks.front();
        }
        const auto laneId = uniqueScoreId("automation");
        if (owner == nullptr || laneId.empty())
            return true;
        talas::project::ScoreAutomationLane lane{
            laneId,
            owner->kind == talas::project::ScoreTrackKind::Master
                ? talas::project::ScoreAutomationScope::Master
                : talas::project::ScoreAutomationScope::Track,
            owner->trackId,
            "Gain",
            talas::expr::ValueDomain::Scalar,
            {},
        };
        const auto result = bridge_.editor().addScoreAutomationLane(
            std::move(lane));
        if (result.ok())
        {
            selection_ = {
                TimelineScoreSelectionKind::AutomationLane, laneId, 0};
            selectedFocusIdentity_ = "timeline.automation:" + laneId;
        }
        return commitDiscreteEdit(result, "Created explicit Gain automation lane");
    }

    [[nodiscard]] bool hasGainAutomationForOwner(
        const talas::project::ScoreTrack& track) const
    {
        const auto expectedScope = track.kind == talas::project::ScoreTrackKind::Master
            ? talas::project::ScoreAutomationScope::Master
            : talas::project::ScoreAutomationScope::Track;
        return std::ranges::any_of(
            bridge_.editor().document().timeline.score.automation,
            [&track, expectedScope](const auto& lane) {
                return lane.ownerId == track.trackId
                    && lane.scope == expectedScope
                    && lane.eventName == "Gain"
                    && lane.domain == talas::expr::ValueDomain::Scalar;
            });
    }

    [[nodiscard]] bool canAddGainAutomation() const
    {
        return std::ranges::any_of(
            bridge_.editor().document().timeline.score.tracks,
            [this](const auto& track) {
                return !hasGainAutomationForOwner(track);
            });
    }

    bool createMarkerAtFrame(std::uint64_t frame, bool acceptSnap)
    {
        if (!timelineScoreHasCommand(
                viewport_.commands, TimelineScoreCommand::AddMarker))
            return false;
        if (frame == std::numeric_limits<std::uint64_t>::max())
        {
            onStatus_("Marker creation rejected outside the half-open Timeline range");
            return true;
        }
        auto domain = talas::project::ScoreTimeDomain::Seconds;
        const auto& score = bridge_.editor().document().timeline.score;
        if (selection_.kind == TimelineScoreSelectionKind::Track)
        {
            const auto track = std::ranges::find(
                score.tracks, selection_.entityId, &talas::project::ScoreTrack::trackId);
            if (track != score.tracks.end())
                domain = track->timeDomain;
        }
        const auto start = timeAtFrame(frame, domain, acceptSnap);
        const auto markerId = uniqueScoreId("marker");
        if (!start.has_value() || markerId.empty())
            return true;
        talas::project::ScoreMarker marker;
        marker.markerId = markerId;
        marker.title = "Marker " + markerId.substr(6);
        marker.start = *start;
        if (!markerProjectsAtActiveRate(marker))
            return true;
        return commitDiscreteEdit(
            bridge_.editor().addScoreMarker(std::move(marker)),
            acceptSnap ? "Created Score marker at accepted snap candidate"
                       : "Created Score marker at exact pointer time");
    }

    bool createMarkerAtPlayhead()
    {
        return createMarkerAtFrame(runtime_.playheadFrame, false);
    }

    bool createClipAtFrame(
        std::string_view trackId,
        std::uint64_t frame,
        bool acceptSnap,
        std::string_view explicitDefinitionId = {})
    {
        if (frame == std::numeric_limits<std::uint64_t>::max())
        {
            onStatus_("Clip creation rejected outside the half-open Timeline range");
            return true;
        }
        const auto& document = bridge_.editor().document();
        const auto track = std::ranges::find(
            document.timeline.score.tracks,
            trackId,
            &talas::project::ScoreTrack::trackId);
        if (track == document.timeline.score.tracks.end()
            || track->kind != talas::project::ScoreTrackKind::Track)
        {
            onStatus_("Sound clips can be created only on a Score track");
            return true;
        }
        auto definitionId = std::string(explicitDefinitionId);
        if (definitionId.empty() && document.soundDefinitions.size() == 1U)
            definitionId = document.soundDefinitions.front().definitionId;
        if (definitionId.empty()
            || std::ranges::none_of(document.soundDefinitions, [&definitionId](const auto& definition) {
                return definition.definitionId == definitionId;
            }))
        {
            onStatus_("Choose one exact Sound from the Score context menu before creating a clip");
            return true;
        }
        const auto typed = talas::app::timelineGridTypesTimes(gridModeValue_);
        auto start = timeAtFrame(typed ? runtime_.playheadFrame : frame, track->timeDomain, acceptSnap);
        // A Magnet grid sticks a Sound to the one
        // before it; with Enter beat or time it starts at the cursor, for its
        // times to be typed in the strip.
        if (start.has_value() && gridModeValue_ == talas::app::TimelineGridMode::Magnet)
            start = talas::app::timelineMagnetStart(document.timeline.score, trackId, *start);
        const auto clipId = uniqueScoreId("clip");
        if (!start.has_value() || clipId.empty())
            return true;
        const auto zero = talas::project::ScoreTime{track->timeDomain, 0.0, 0};
        const auto duration = track->timeDomain
                == talas::project::ScoreTimeDomain::Seconds
            ? talas::project::ScoreTime{track->timeDomain, 0.5, 0}
            : talas::project::ScoreTime{track->timeDomain, 0.0, 480};
        talas::project::ScoreClip clip{
            clipId,
            std::string(trackId),
            definitionId,
            *start,
            duration,
            zero,
            zero,
            zero,
            1,
        };
        if (!clipProjectsAtActiveRate(clip))
        {
            onStatus_("Clip creation exceeds the active Timeline sample range");
            return true;
        }
        const auto added = bridge_.editor().addScoreClip(std::move(clip));
        (void) commitDiscreteEdit(added,
            acceptSnap ? "Created Score clip at accepted snap candidate"
                       : "Created Score clip at exact pointer time");
        // The Sound put into the Timeline is
        // selected, its information shown.
        if (added.ok())
        {
            selectClips({clipId});
            refreshInfoStrip();
            if (typed)
                infoStrip_.beginTyping(TimelineInfoField::SoundStart);
            repaint();
        }
        return true;
    }

    bool addAutomationPointAtFrame(std::uint64_t frame, bool acceptSnap, std::optional<double> height = {})
    {
        if (frame == std::numeric_limits<std::uint64_t>::max())
        {
            onStatus_("Automation point rejected outside the half-open Timeline range");
            return true;
        }
        if (selection_.kind != TimelineScoreSelectionKind::AutomationLane
            && selection_.kind != TimelineScoreSelectionKind::AutomationPoint)
        {
            return false;
        }
        const auto& score = bridge_.editor().document().timeline.score;
        const auto found = std::ranges::find(
            score.automation, selection_.entityId,
            &talas::project::ScoreAutomationLane::laneId);
        if (found == score.automation.end())
            return false;
        auto lane = *found;
        auto domain = lane.points.empty()
            ? talas::project::ScoreTimeDomain::Seconds
            : lane.points.front().position.domain;
        if (lane.scope == talas::project::ScoreAutomationScope::Clip)
        {
            const auto owner = std::ranges::find(
                score.clips, lane.ownerId, &talas::project::ScoreClip::clipId);
            if (owner == score.clips.end())
                return true;
            domain = owner->start.domain;
        }
        else if (lane.points.empty())
        {
            const auto owner = std::ranges::find(
                score.tracks, lane.ownerId, &talas::project::ScoreTrack::trackId);
            if (owner == score.tracks.end())
                return true;
            domain = owner->timeDomain;
        }
        auto position = timeAtFrame(frame, domain, acceptSnap);
        if (!position.has_value())
            return true;
        if (lane.scope == talas::project::ScoreAutomationScope::Clip)
        {
            const auto owner = std::ranges::find(
                score.clips, lane.ownerId, &talas::project::ScoreClip::clipId);
            const auto local = owner != score.clips.end()
                ? subtractScoreTime(*position, owner->start)
                : std::nullopt;
            if (!local.has_value())
                return true;
            position = *local;
        }
        auto value = lane.points.empty() ? 0.0 : lane.points.back().value;
        if (height.has_value())
        {
            // At the value clicked, as the lane shows its points,
            // zoomed out or in.
            const auto zoom = laneValueZoom(lane.laneId);
            auto added = talas::app::addAutomationPoint(lane, *position,
                zoom == 0 ? talas::app::automationValueAtHeight(lane, *height)
                          : talas::app::automationValueAtHeight(talas::app::automationValueRange(lane.points, zoom),
                                std::clamp(*height, 0.0, 1.0)));
            if (!added.has_value())
                return true;
            lane = std::move(*added);
        }
        else
        {
            lane.points.push_back({*position, value,
                talas::core::AutomationInterpolation::Step});
            std::ranges::stable_sort(lane.points, [](const auto& left, const auto& right) {
                if (left.position.domain == talas::project::ScoreTimeDomain::Seconds)
                    return left.position.seconds < right.position.seconds;
                return left.position.tick < right.position.tick;
            });
        }
        if (!laneProjectsAtActiveRate(lane))
        {
            onStatus_("Automation point exceeds the active Timeline sample range");
            return true;
        }
        const auto inserted = std::ranges::find(
            lane.points, *position, &talas::project::ScoreAutomationPoint::position);
        const auto insertedIndex = inserted == lane.points.end()
            ? lane.points.size()
            : static_cast<std::size_t>(std::distance(lane.points.begin(), inserted));
        const auto result = bridge_.editor().updateScoreAutomationLane(lane);
        if (result.ok() && insertedIndex < lane.points.size())
        {
            selection_ = {
                TimelineScoreSelectionKind::AutomationPoint,
                lane.laneId,
                insertedIndex,
            };
            selectedPointPosition_ = *position;
        }
        return commitDiscreteEdit(
            result,
            acceptSnap ? "Created Score automation point at accepted snap candidate"
                       : "Created Score automation point at exact pointer time");
    }

    bool addAutomationPointAtPlayhead()
    {
        return addAutomationPointAtFrame(runtime_.playheadFrame, false);
    }

    bool adjustSelectedClipRepeat(int delta)
    {
        if (selection_.kind != TimelineScoreSelectionKind::Clip)
            return false;
        const auto& clips = bridge_.editor().document().timeline.score.clips;
        const auto found = std::ranges::find(
            clips, selection_.entityId, &talas::project::ScoreClip::clipId);
        if (found == clips.end())
            return false;
        auto clip = *found;
        if (delta > 0 && clip.repeatCount < talas::project::maximumScoreClipRepeats)
            ++clip.repeatCount;
        else if (delta < 0 && clip.repeatCount > 1U)
            --clip.repeatCount;
        else
            return true;
        if (!clipProjectsAtActiveRate(clip))
        {
            onStatus_("Clip repeat exceeds the active Timeline sample range");
            return true;
        }
        return commitDiscreteEdit(
            bridge_.editor().updateScoreClip(std::move(clip)),
            "Updated Score clip repeat count");
    }

    bool resizeSelectedClipEnd(int direction)
    {
        if (selection_.kind != TimelineScoreSelectionKind::Clip)
            return false;
        const auto& clips = bridge_.editor().document().timeline.score.clips;
        const auto found = std::ranges::find(
            clips, selection_.entityId, &talas::project::ScoreClip::clipId);
        if (found == clips.end())
            return false;
        auto clip = *found;
        const auto total = multiplyScoreTime(clip.duration, clip.repeatCount);
        const auto adjusted = total.has_value()
            ? direction > 0
                ? addScoreTime(*total, nudgeAmount(clip.duration.domain, false))
                : subtractScoreTime(*total, nudgeAmount(clip.duration.domain, false))
            : std::nullopt;
        const auto duration = adjusted.has_value()
            ? divideScoreTime(*adjusted, clip.repeatCount)
            : std::nullopt;
        if (!duration.has_value())
            return true;
        clip.duration = *duration;
        const auto repeated = multiplyScoreTime(clip.duration, clip.repeatCount);
        if (!repeated.has_value() || !clipProjectsAtActiveRate(clip))
            return true;
        if (!clampClipFadesToTotalDuration(clip, *repeated))
            return true;
        return commitDiscreteEdit(
            bridge_.editor().updateScoreClip(std::move(clip)),
            direction > 0 ? "Extended exact Score clip end"
                          : "Shortened exact Score clip end");
    }

    bool trimSelectedClipStart(int direction)
    {
        if (selection_.kind != TimelineScoreSelectionKind::Clip)
            return false;
        const auto& clips = bridge_.editor().document().timeline.score.clips;
        const auto found = std::ranges::find(
            clips, selection_.entityId, &talas::project::ScoreClip::clipId);
        if (found == clips.end())
            return false;
        auto clip = *found;
        const auto step = nudgeAmount(clip.start.domain, false);
        const auto total = multiplyScoreTime(clip.duration, clip.repeatCount);
        const auto end = total.has_value()
            ? addScoreTime(clip.start, *total)
            : std::nullopt;
        const auto nextStart = direction > 0
            ? addScoreTime(clip.start, step)
            : subtractScoreTime(clip.start, step);
        if (!end.has_value() || !nextStart.has_value())
            return true;
        const auto nextTotal = subtractScoreTime(*end, *nextStart);
        const auto nextDuration = nextTotal.has_value()
            ? divideScoreTime(*nextTotal, clip.repeatCount)
            : std::nullopt;
        const auto nextOffset = direction > 0
            ? addScoreTime(clip.contentOffset, step)
            : subtractScoreTime(clip.contentOffset, step);
        if (!nextDuration.has_value() || !nextOffset.has_value())
            return true;
        clip.start = *nextStart;
        clip.duration = *nextDuration;
        clip.contentOffset = *nextOffset;
        const auto repeated = multiplyScoreTime(clip.duration, clip.repeatCount);
        if (!repeated.has_value() || !clipProjectsAtActiveRate(clip))
            return true;
        if (!clampClipFadesToTotalDuration(clip, *repeated))
            return true;
        return commitDiscreteEdit(
            bridge_.editor().updateScoreClip(std::move(clip)),
            direction > 0 ? "Trimmed exact Score clip start"
                          : "Extended exact Score clip start");
    }

    bool seekToSelection()
    {
        if (runtime_.syncKind != TimelineExternalSyncKind::Internal)
        {
            onStatus_("Seek is held while an external transport owns position");
            return selection_.kind != TimelineScoreSelectionKind::None;
        }
        const auto& score = bridge_.editor().document().timeline.score;
        std::optional<talas::project::ScoreTime> time;
        if (selection_.kind == TimelineScoreSelectionKind::Clip)
        {
            const auto found = std::ranges::find(
                score.clips, selection_.entityId, &talas::project::ScoreClip::clipId);
            if (found != score.clips.end())
                time = found->start;
        }
        else if (selection_.kind == TimelineScoreSelectionKind::Marker)
        {
            const auto found = std::ranges::find(
                score.markers, selection_.entityId,
                &talas::project::ScoreMarker::markerId);
            if (found != score.markers.end())
                time = found->start;
        }
        else if (selection_.kind == TimelineScoreSelectionKind::AutomationPoint)
        {
            const auto lane = std::ranges::find(
                score.automation, selection_.entityId,
                &talas::project::ScoreAutomationLane::laneId);
            if (lane != score.automation.end()
                && selection_.pointIndex < lane->points.size())
            {
                time = lane->points[selection_.pointIndex].position;
                if (lane->scope == talas::project::ScoreAutomationScope::Clip)
                {
                    const auto owner = std::ranges::find(
                        score.clips, lane->ownerId,
                        &talas::project::ScoreClip::clipId);
                    time = owner != score.clips.end()
                        ? addScoreTime(owner->start, *time)
                        : std::nullopt;
                }
            }
        }
        const auto frame = time.has_value() ? activeFrameFor(*time) : std::nullopt;
        if (!frame.has_value() || !bridge_.requestTransportRelocation(*frame))
        {
            onStatus_("Selected Score position could not be queued for seek");
            return time.has_value();
        }
        onStatus_("Queued selected Score position at the next audio boundary");
        return true;
    }

    bool toggleSelectedTrackMute()
    {
        if (!timelineScoreHasCommand(
                viewport_.commands, TimelineScoreCommand::ToggleMute))
            return false;
        if (selection_.kind != TimelineScoreSelectionKind::Track)
            return false;
        const auto& tracks = bridge_.editor().document().timeline.score.tracks;
        const auto found = std::ranges::find(
            tracks, selection_.entityId, &talas::project::ScoreTrack::trackId);
        if (found == tracks.end())
            return false;
        auto track = *found;
        track.muted = !track.muted;
        return commitDiscreteEdit(
            bridge_.editor().updateScoreTrack(std::move(track)),
            "Toggled Score track mute");
    }

    bool toggleSelectedTrackSolo()
    {
        if (!timelineScoreHasCommand(
                viewport_.commands, TimelineScoreCommand::ToggleSolo))
            return false;
        if (selection_.kind != TimelineScoreSelectionKind::Track)
            return false;
        const auto& tracks = bridge_.editor().document().timeline.score.tracks;
        const auto found = std::ranges::find(
            tracks, selection_.entityId, &talas::project::ScoreTrack::trackId);
        if (found == tracks.end())
            return false;
        auto track = *found;
        track.solo = !track.solo;
        return commitDiscreteEdit(
            bridge_.editor().updateScoreTrack(std::move(track)),
            "Toggled Score track solo");
    }

    // A lane for any control of a track, the
    // master or a clip; curves between points; the controller
    // transformations.
    enum AutomationMenuItem
    {
        automateBase = 7000,
        curveExponential = 7100,
        curveLogarithmic,
        curveSCurve,
        transformInvert = 7200,
        transformReverse,
        transformLoop,
        transformSquareOff,
        transformScale,
        transformOffset,
        transformThreshold,
        transformTimeScale,
        transformSpray,
        transformQuantize,
        transformScaleToFit,
        transformKeep,
        laneStateBase = timelineLaneStateMenuBase,
        groupStateBase = timelineAutomationMenuBase,
        alignBase = 7400,
        inputMenuBase = 7900,
        hardwareInputMenuBase = 8400,
        submixMenuBase = 8500,
        takeMenuBase = 9100,
        outputMenuBase = 9500,
        masterMenuBase = 9300,
        midiMenuBase = 9600,
    };
    static constexpr std::size_t maximumAlignTargets = 200;

    // The Lane State menu's Automated, Live and Record Enable, and the
    // Automation menu's states for Parameter Controls and Track Level &
    // Angle Controls, in turn.
    bool performLaneStateMenuAction(int actionId)
    {
        if (actionId >= laneStateBase && actionId <= laneStateBase + 2)
            return setSelectedLaneState(static_cast<talas::project::ScoreAutomationState>(actionId - laneStateBase));
        if (actionId == groupStateBase + 6)
        {
            askMasterBpm();
            return true;
        }
        if (actionId >= groupStateBase && actionId <= groupStateBase + 5)
            return setGroupState(
                (actionId - groupStateBase) % 2 == 0 ? talas::app::AutomationControlGroup::ParameterControls
                                                     : talas::app::AutomationControlGroup::TrackLevelAndAngle,
                static_cast<talas::project::ScoreAutomationState>((actionId - groupStateBase) / 2));
        return false;
    }

    [[nodiscard]] static juce::String automationTargetLabel(
        const talas::app::AutomationTarget& target)
    {
        auto label = juceString(target.label.empty() ? target.eventName : target.label);
        switch (target.domain)
        {
        case talas::expr::ValueDomain::Hertz:
            label += " (hertz)";
            break;
        case talas::expr::ValueDomain::Seconds:
            label += " (seconds)";
            break;
        default:
            break;
        }
        return label;
    }

    struct AutomationOwner
    {
        talas::project::ScoreAutomationScope scope = talas::project::ScoreAutomationScope::Track;
        std::string ownerId;
    };

    [[nodiscard]] std::optional<AutomationOwner> selectedAutomationOwner() const
    {
        const auto& score = bridge_.editor().document().timeline.score;
        if (selection_.kind == TimelineScoreSelectionKind::Track)
        {
            const auto track = std::ranges::find(
                score.tracks, selection_.entityId, &talas::project::ScoreTrack::trackId);
            if (track == score.tracks.end())
                return std::nullopt;
            return AutomationOwner{
                track->kind == talas::project::ScoreTrackKind::Master
                    ? talas::project::ScoreAutomationScope::Master
                    : talas::project::ScoreAutomationScope::Track,
                track->trackId};
        }
        if (selection_.kind == TimelineScoreSelectionKind::Clip)
        {
            const auto clip = std::ranges::find(
                score.clips, selection_.entityId, &talas::project::ScoreClip::clipId);
            if (clip == score.clips.end())
                return std::nullopt;
            return AutomationOwner{talas::project::ScoreAutomationScope::Clip, clip->clipId};
        }
        return std::nullopt;
    }

    [[nodiscard]] std::vector<talas::app::AutomationTarget> selectedAutomationTargets() const
    {
        const auto owner = selectedAutomationOwner();
        if (!owner.has_value())
            return {};
        return talas::app::automationTargets(
            bridge_.editor().document(), owner->scope, owner->ownerId);
    }

    bool addAutomationLaneFor(
        talas::project::ScoreAutomationScope scope,
        std::string ownerId,
        const talas::app::AutomationTarget& target)
    {
        const auto laneId = uniqueScoreId("automation");
        if (laneId.empty())
            return false;
        talas::project::ScoreAutomationLane lane{
            laneId, scope, std::move(ownerId), target.eventName, target.domain, {}};
        const auto result = bridge_.editor().addScoreAutomationLane(std::move(lane));
        if (result.ok())
        {
            selection_ = {TimelineScoreSelectionKind::AutomationLane, laneId, 0};
            selectedFocusIdentity_ = "timeline.automation:" + laneId;
        }
        return commitDiscreteEdit(
            result, "Created " + automationTargetLabel(target) + " automation lane");
    }

    bool commitAutomationLaneEdit(
        const std::optional<talas::project::ScoreAutomationLane>& edited,
        juce::String success)
    {
        if (!edited.has_value())
        {
            onStatus_("That automation edit does not apply to the selected lane");
            return true;
        }
        if (!laneProjectsAtActiveRate(*edited))
        {
            onStatus_("The edited automation exceeds the active Timeline sample range");
            return true;
        }
        if (selection_.kind == TimelineScoreSelectionKind::AutomationPoint
            && selection_.pointIndex >= edited->points.size())
        {
            selection_ = {TimelineScoreSelectionKind::AutomationLane, edited->laneId, 0};
        }
        return commitDiscreteEdit(
            bridge_.editor().updateScoreAutomationLane(*edited), std::move(success));
    }

    bool shapeSelectedAutomationSegment(talas::app::AutomationCurveShape shape)
    {
        const auto* lane = selectedAutomationLane();
        if (lane == nullptr || selection_.kind != TimelineScoreSelectionKind::AutomationPoint)
            return false;
        return commitAutomationLaneEdit(
            talas::app::shapeAutomationSegment(*lane, selection_.pointIndex, shape),
            "Curved the automation to its next point");
    }

    bool transformSelectedAutomationLane(
        talas::app::AutomationTransform transform,
        talas::app::AutomationTransformParameters parameters)
    {
        const auto* lane = selectedAutomationLane();
        if (lane == nullptr)
            return false;
        // The button pressed, or pressed
        // again to take it away.
        const auto pressed = talas::app::automationLaneTransformed(*lane, transform);
        return commitAutomationLaneEdit(
            talas::app::automationLaneWithTransformToggled(*lane, transform, parameters),
            pressed ? "Took the transformation away" : "Transformed the automation lane");
    }

    // The function the lane plays, transformed or slaved, kept as
    // its own points.
    bool keepSelectedAutomationFunction()
    {
        const auto* lane = selectedAutomationLane();
        if (lane == nullptr)
            return false;
        return commitAutomationLaneEdit(
            talas::app::automationLaneWithFunctionKept(bridge_.editor().document().timeline.score, lane->laneId),
            "Kept the transformed function as the lane's points");
    }

    // Scale to fit, the lane fitted once within its
    // clip's duration.
    bool scaleSelectedAutomationLaneToItsClip()
    {
        const auto* lane = selectedAutomationLane();
        if (lane == nullptr || lane->scope != talas::project::ScoreAutomationScope::Clip)
            return false;
        const auto& clips = bridge_.editor().document().timeline.score.clips;
        const auto clip = std::ranges::find(clips, lane->ownerId, &talas::project::ScoreClip::clipId);
        if (clip == clips.end())
            return false;
        return transformSelectedAutomationLane(talas::app::AutomationTransform::ScaleToFit, {laneTime(clip->duration), 0.0, 1});
    }

    // Set selected Sounds' live controls to current
    // values, each VCS control a constant lane of the clip.
    bool setSelectedClipLiveControlsToCurrentValues()
    {
        const auto& document = bridge_.editor().document();
        std::vector<talas::app::LiveControlValue> values;
        for (const auto& control : document.virtualControls)
            values.push_back({control.eventName, control.domain, control.value});
        auto lanes = talas::app::liveControlsAsLanes(document.timeline.score, selection_.entityId, values);
        if (!lanes.has_value() || lanes->empty())
        {
            onStatus_("No live controls to set on the selected clip");
            return true;
        }
        talas::project::ProjectScoreEditBatch edits;
        edits.automation = std::move(*lanes);
        return commitDiscreteEdit(bridge_.editor().applyScoreEdits(std::move(edits)),
            "Set the clip's live controls to their current values");
    }

    // A lane's state: Live, Automated or
    // Record-enabled.
    bool armLiveLanesForCache(const std::string& clipId)
    {
        auto lanes = talas::app::liveLanesArmedForCache(bridge_.editor().document().timeline.score, clipId);
        if (lanes.empty())
            return true;
        talas::project::ProjectScoreEditBatch edits;
        edits.automation = std::move(lanes);
        return commitDiscreteEdit(bridge_.editor().applyScoreEdits(std::move(edits)),
            "Armed the Sound's live controls for recording as it caches");
    }

    bool setSelectedLaneState(talas::project::ScoreAutomationState state)
    {
        const auto* lane = selectedAutomationLane();
        if (lane == nullptr)
            return false;
        auto edited = *lane;
        edited.state = state;
        talas::project::ProjectScoreEditBatch edits;
        edits.automation = {std::move(edited)};
        return commitDiscreteEdit(bridge_.editor().applyScoreEdits(std::move(edits)),
            juce::String{"Set the lane "} + laneStateName(state));
    }

    // The Automation menu: the selected clip's
    // parameter controls, or its track's level and angle, set to one state;
    // with no clip selected, every clip's and every track's.
    bool setGroupState(talas::app::AutomationControlGroup group, talas::project::ScoreAutomationState state)
    {
        const auto& score = bridge_.editor().document().timeline.score;
        std::optional<std::vector<std::string>> clips;
        if (selection_.kind == TimelineScoreSelectionKind::Clip)
            clips = std::vector<std::string>{selection_.entityId};
        auto lanes = talas::app::lanesInState(score, clips, group, state);
        if (lanes.empty())
        {
            onStatus_("No lanes to change");
            return true;
        }
        talas::project::ProjectScoreEditBatch edits;
        edits.automation = std::move(lanes);
        return commitDiscreteEdit(bridge_.editor().applyScoreEdits(std::move(edits)),
            juce::String{group == talas::app::AutomationControlGroup::ParameterControls ? "Parameter controls " : "Track level and angle controls "}
                + laneStateName(state));
    }

    [[nodiscard]] static const char* laneStateName(talas::project::ScoreAutomationState state)
    {
        switch (state)
        {
        case talas::project::ScoreAutomationState::Live: return "live";
        case talas::project::ScoreAutomationState::Record: return "record-enabled";
        case talas::project::ScoreAutomationState::Automated: break;
        }
        return "automated";
    }

    // The strip at the top of the Timeline.
    [[nodiscard]] const talas::project::ScoreClip* selectedClip() const
    {
        if (selection_.kind != TimelineScoreSelectionKind::Clip)
            return nullptr;
        const auto& clips = bridge_.editor().document().timeline.score.clips;
        const auto found = std::ranges::find(clips, selection_.entityId, &talas::project::ScoreClip::clipId);
        return found == clips.end() ? nullptr : &*found;
    }

    [[nodiscard]] const talas::project::ScoreMarker* selectedMarker() const
    {
        if (selection_.kind != TimelineScoreSelectionKind::Marker)
            return nullptr;
        const auto& markers = bridge_.editor().document().timeline.score.markers;
        const auto found = std::ranges::find(markers, selection_.entityId, &talas::project::ScoreMarker::markerId);
        return found == markers.end() ? nullptr : &*found;
    }

    [[nodiscard]] std::uint32_t activeSampleRate() const noexcept
    {
        return std::max<std::uint32_t>(1, runtime_.sampleRate);
    }

    void refreshInfoStrip()
    {
        const auto& document = bridge_.editor().document();
        const auto ownFrames = document.timeline.timeCodeFramesPerSecond;
        framesMenu_.setSelectedId(ownFrames == 0 ? 1 : ownFrames, juce::dontSendNotification);
        TimelineInfoStrip::State state;
        state.musicalTime = document.timeline.musicalTime;
        const auto milliseconds = std::round(
            static_cast<double>(runtime_.playheadFrame) * 1000.0 / static_cast<double>(activeSampleRate()));
        state.cursor = talas::app::timelineTimeText(
            {talas::project::ScoreTimeDomain::Seconds, milliseconds / 1000.0, 0}, state.musicalTime);
        if (const auto* clip = selectedClip())
        {
            state.sound = selectedClipIds().size() > 1 ? talas::app::timelineGroupInfo(document, selectedClipIds())
                                                       : talas::app::timelineSoundInfo(document, clip->clipId);
            state.soundColour = clip->colour;
        }
        if (const auto* marker = selectedMarker())
        {
            state.marker = *marker;
            state.markerProgram = talas::app::markerProgramNumber(document, activeSampleRate(), *marker);
        }
        // The earliest selected point, at its time in
        // the Timeline.
        if (const auto* lane = selectedPointLane(); lane != nullptr && !selectedPointIndices().empty())
        {
            const auto& point = lane->points[selectedPointIndices().front()];
            state.pointTime = point.position;
            state.pointValue = point.value;
            if (const auto start = pointLaneStart(*lane))
                state.pointTime = addScoreTime(point.position, *start);
        }
        infoStrip_.show(state);
        programChannel_.setSelectedId(document.timeline.markerProgramChannel + 1, juce::dontSendNotification);
    }

    bool commitInfoEdit(const talas::project::EditResult& result, juce::String successStatus)
    {
        if (!result.ok())
        {
            onStatus_(juceString(result.message));
            return false;
        }
        onDocumentChange_();
        rebuild(bridge_.editor().document(), runtime_);
        onStatus_(std::move(successStatus));
        return true;
    }

    bool seekToFrame(std::uint64_t frame)
    {
        if (runtime_.syncKind != TimelineExternalSyncKind::Internal)
        {
            onStatus_("Seek is held while an external transport owns position");
            return false;
        }
        if (!bridge_.requestTransportRelocation(frame))
        {
            onStatus_("Timeline seek could not be queued");
            return false;
        }
        onStatus_("Queued exact Timeline seek at the next audio boundary");
        return true;
    }

    // A clip lane's points are timed from its clip's start.
    [[nodiscard]] std::optional<talas::project::ScoreTime> pointLaneStart(const talas::project::ScoreAutomationLane& lane) const
    {
        if (lane.scope != talas::project::ScoreAutomationScope::Clip)
            return std::nullopt;
        const auto& clips = bridge_.editor().document().timeline.score.clips;
        const auto clip = std::ranges::find(clips, lane.ownerId, &talas::project::ScoreClip::clipId);
        return clip == clips.end() ? std::nullopt : std::optional<talas::project::ScoreTime>{clip->start};
    }

    bool applyInfoField(TimelineInfoField field, const std::string& text)
    {
        const auto& document = bridge_.editor().document();
        const auto& musical = document.timeline.musicalTime;
        if (field == TimelineInfoField::PointValue)
            return setSelectedPointsValue(text);
        if (field == TimelineInfoField::PointTime)
        {
            const auto* lane = selectedPointLane();
            if (lane == nullptr || selectedPointIndices().empty())
                return false;
            const auto domain = lane->points[selectedPointIndices().front()].position.domain;
            auto time = talas::app::parseTimelineTime(text, domain, musical);
            if (time.has_value())
            {
                if (const auto start = pointLaneStart(*lane))
                    time = subtractScoreTime(*time, *start);
            }
            if (!time.has_value())
            {
                onStatus_("Type a time within the control's Sound");
                return false;
            }
            return moveSelectedPointsTo(talas::app::automationPointTime(*time));
        }
        if (field == TimelineInfoField::Cursor)
        {
            const auto time = talas::app::parseTimelineTime(text, talas::project::ScoreTimeDomain::Seconds, musical);
            if (!time.has_value())
            {
                onStatus_("Type a time for the cursor, such as 12.5, 1:02.5 or 00:01:02:15");
                return false;
            }
            return seekToFrame(static_cast<std::uint64_t>(std::llround(time->seconds * activeSampleRate())));
        }
        if (field == TimelineInfoField::MarkerName || field == TimelineInfoField::MarkerTime)
        {
            const auto* marker = selectedMarker();
            if (marker == nullptr)
                return false;
            auto edited = *marker;
            if (field == TimelineInfoField::MarkerName)
            {
                if (text.empty())
                    return false;
                edited.title = text;
                return commitInfoEdit(bridge_.editor().updateScoreMarker(std::move(edited)), "Renamed the marker");
            }
            const auto time = talas::app::parseTimelineTime(text, marker->start.domain, musical);
            if (!time.has_value())
            {
                onStatus_("Type a time for the marker");
                return false;
            }
            if (edited.end.has_value())
            {
                const auto length = subtractScoreTime(*edited.end, edited.start);
                const auto end = length.has_value() ? addScoreTime(*time, *length) : std::nullopt;
                if (!end.has_value())
                    return false;
                edited.end = *end;
            }
            edited.start = *time;
            if (!markerProjectsAtActiveRate(edited))
                return false;
            return commitInfoEdit(bridge_.editor().updateScoreMarker(std::move(edited)), "Moved the marker to the time typed");
        }
        const auto* clip = selectedClip();
        if (clip == nullptr)
            return false;
        // A group's start moves the whole group.
        if (const auto ids = selectedClipIds(); ids.size() > 1)
        {
            const auto& score = document.timeline.score;
            const auto first = talas::app::earliestClip(score, ids);
            const auto earliest = std::ranges::find(score.clips, *first, &talas::project::ScoreClip::clipId);
            if (field != TimelineInfoField::SoundStart)
            {
                onStatus_("A group of Sounds takes a new start only");
                return false;
            }
            const auto time = talas::app::parseTimelineTime(text, earliest->start.domain, musical);
            const auto group = time.has_value() ? talas::app::clipsMovedTogether(score, ids, *time) : std::nullopt;
            if (!group.has_value())
            {
                onStatus_("Type a time the group can start at");
                return false;
            }
            return commitClipGroup(*group, "Moved the selected Sounds");
        }
        const auto soundField = field == TimelineInfoField::SoundStart ? talas::app::TimelineSoundField::Start
            : field == TimelineInfoField::SoundAnchor                  ? talas::app::TimelineSoundField::Anchor
            : field == TimelineInfoField::SoundDuration                ? talas::app::TimelineSoundField::Duration
                                                                       : talas::app::TimelineSoundField::End;
        const auto time = talas::app::parseTimelineTime(
            text, clip->start.domain, musical, soundField == talas::app::TimelineSoundField::Duration);
        const auto edited = time.has_value() ? talas::app::clipWithSoundField(*clip, soundField, *time) : std::nullopt;
        if (!edited.has_value())
        {
            onStatus_(soundField == talas::app::TimelineSoundField::Anchor
                    ? "The anchor must be a time within the Sound"
                    : "Type a time the Sound can take");
            return false;
        }
        talas::project::ProjectScoreEditBatch edits;
        edits.clips = {*edited};
        return commitInfoEdit(bridge_.editor().applyScoreEdits(std::move(edits)), "Changed the Sound's times");
    }

    bool setSelectedMarkerProgram(std::uint8_t program)
    {
        const auto* marker = selectedMarker();
        if (marker == nullptr)
            return false;
        auto edited = talas::app::markerWithProgramNumber(bridge_.editor().document(), activeSampleRate(), *marker, program);
        return commitInfoEdit(bridge_.editor().updateScoreMarker(std::move(edited)),
            juce::String("Program change ") + juce::String(program) + " jumps to the marker");
    }

    bool setMarkerProgramChannel(std::uint8_t channel)
    {
        return commitInfoEdit(bridge_.editor().setTimelineMarkerProgramChannel(channel),
            channel == 0 ? juce::String("No MIDI channel's program changes jump to markers")
                         : juce::String("Program changes on channel ") + juce::String(channel) + " jump to markers");
    }

    // Tab and Shift+Tab in a time field: the next or previous of the
    // time's units selected to type over.
    struct TimeUnitTabs final : juce::KeyListener
    {
        bool keyPressed(const juce::KeyPress& key, juce::Component* component) override
        {
            auto* field = dynamic_cast<juce::TextEditor*>(component);
            if (field == nullptr || key.getKeyCode() != juce::KeyPress::tabKey)
                return false;
            const auto text = field->getText().toStdString();
            const auto caret = static_cast<std::size_t>(std::max(0, field->getHighlightedRegion().getStart()));
            if (const auto unit = talas::app::adjacentTimelineTimeUnit(text, caret, !key.getModifiers().isShiftDown()))
                field->setHighlightedRegion({static_cast<int>(unit->start), static_cast<int>(unit->start + unit->length)});
            return true;
        }
    };

    // The Marker button's dialog: a typed time and OK, or Enter or
    // Capture for the cursor's time, even while paused.
    void showMarkerDialog()
    {
        auto* window = new juce::AlertWindow("Marker",
            "Type the marker's time and click OK, or press Enter or Capture to place it at the cursor; Tab goes from one of its units to the next.",
            juce::MessageBoxIconType::NoIcon, this);
        window->addTextEditor("time", juceString(infoStrip_.fieldText(TimelineInfoField::Cursor)), "Time");
        auto* tabs = new TimeUnitTabs;
        window->getTextEditor("time")->addKeyListener(tabs);
        window->addButton("OK", 1);
        window->addButton("Capture", 2, juce::KeyPress{juce::KeyPress::returnKey});
        window->addButton("Cancel", 0, juce::KeyPress{juce::KeyPress::escapeKey});
        juce::Component::SafePointer<ScoreViewportComponent> safeThis(this);
        window->enterModalState(
            true,
            juce::ModalCallbackFunction::create([safeThis, window, tabs](int result) {
                window->getTextEditor("time")->removeKeyListener(tabs);
                delete tabs;
                if (safeThis == nullptr)
                    return;
                const auto answer = result == 2 ? talas::app::MarkerDialogAnswer::Capture
                    : result == 1               ? talas::app::MarkerDialogAnswer::Ok
                                                : talas::app::MarkerDialogAnswer::Cancel;
                if (answer == talas::app::MarkerDialogAnswer::Cancel)
                    return;
                const auto frame = talas::app::markerDialogFrame(answer, window->getTextEditorContents("time").toStdString(),
                    safeThis->runtime_.playheadFrame, safeThis->bridge_.editor().document().timeline.musicalTime,
                    safeThis->activeSampleRate());
                if (frame.has_value())
                    (void) safeThis->createMarkerAtFrame(*frame, false);
                else
                    safeThis->onStatus_("Type a time for the marker");
            }),
            true);
    }

    // The Anchor button: the selected Sound's anchor at the cursor.
    bool anchorSelectedClipAtCursor()
    {
        const auto* clip = selectedClip();
        if (clip == nullptr)
        {
            onStatus_("Select a Sound to anchor");
            return false;
        }
        const auto time = timeAtFrame(runtime_.playheadFrame, clip->start.domain, false);
        const auto edited = time.has_value()
            ? talas::app::clipWithSoundField(*clip, talas::app::TimelineSoundField::Anchor, *time)
            : std::nullopt;
        if (!edited.has_value())
        {
            onStatus_("The cursor is outside the selected Sound");
            return false;
        }
        talas::project::ProjectScoreEditBatch edits;
        edits.clips = {*edited};
        return commitInfoEdit(bridge_.editor().applyScoreEdits(std::move(edits)), "Anchored the Sound at the cursor");
    }

    // Align Start or End to Anchor of: the selected Sound begins or
    // ends at another's anchor.
    bool alignSelectedClip(std::string_view anchoredClipId, talas::app::ClipAlignment alignment)
    {
        const auto* clip = selectedClip();
        const auto& clips = bridge_.editor().document().timeline.score.clips;
        const auto anchored = std::ranges::find(clips, anchoredClipId, &talas::project::ScoreClip::clipId);
        if (clip == nullptr || anchored == clips.end())
            return false;
        const auto aligned = talas::app::clipAlignedTo(*clip, *anchored, alignment);
        if (!aligned.has_value())
        {
            onStatus_("The Sound cannot be aligned to that anchor");
            return true;
        }
        talas::project::ProjectScoreEditBatch edits;
        edits.clips = {*aligned};
        return commitDiscreteEdit(bridge_.editor().applyScoreEdits(std::move(edits)),
            alignment == talas::app::ClipAlignment::StartToAnchor ? "Aligned the Sound to begin at the anchor"
                                                               : "Aligned the Sound to end at the anchor");
    }

    // A MIDI program change on the Timeline's channel jumps the
    // cursor to its marker; on another channel it is left to the presets.
    bool jumpToMarkerForProgramChange(std::uint8_t channel, std::uint8_t program)
    {
        const auto& document = bridge_.editor().document();
        if (!talas::app::timelineTakesProgramChange(document, channel))
            return false;
        const auto* marker = talas::app::markerForProgramChange(document, activeSampleRate(), program);
        const auto frame = marker == nullptr
            ? std::nullopt
            : talas::project::scoreTimeToSampleFrame(marker->start, document.timeline.musicalTime, activeSampleRate());
        if (!frame.has_value())
        {
            onStatus_(juce::String("No marker answers program change ") + juce::String(program));
            return true;
        }
        (void) seekToFrame(*frame);
        return true;
    }

    // 0 and 1 the marker before and after the
    // cursor, 2 and 3 the important time before and after it.
    bool jumpCursor(int which)
    {
        const auto& document = bridge_.editor().document();
        const auto prepared = talas::runtime::prepareProjectScore(document,
            talas::runtime::preparedScoreBindingForDocument(document, 1, 1, std::max<std::uint32_t>(1, runtime_.sampleRate), 1));
        if (!prepared.ok())
            return false;
        const auto cursor = runtime_.playheadFrame;
        const auto frame = which == 0 ? talas::runtime::previousScoreMarkerFrame(*prepared.schedule, cursor)
            : which == 1             ? talas::runtime::nextScoreMarkerFrame(*prepared.schedule, cursor)
            : which == 2             ? talas::runtime::previousScoreImportantFrame(*prepared.schedule, cursor)
                                     : talas::runtime::nextScoreImportantFrame(*prepared.schedule, cursor);
        if (!frame.has_value() || !bridge_.requestTransportRelocation(*frame))
        {
            onStatus_("Nothing to jump to from the cursor");
            return true;
        }
        onStatus_("Queued the jump at the next audio boundary");
        return true;
    }

    [[nodiscard]] static double laneTime(const talas::project::ScoreTime& time) noexcept
    {
        return time.domain == talas::project::ScoreTimeDomain::Seconds
            ? time.seconds
            : static_cast<double>(time.tick);
    }

    // The transformations that take a number ask for it, with a default
    // drawn from the lane.
    void promptAutomationTransform(int item)
    {
        const auto* lane = selectedAutomationLane();
        if (lane == nullptr || lane->points.empty())
            return;
        const auto first = laneTime(lane->points.front().position);
        const auto last = laneTime(lane->points.back().position);
        const auto unit = lane->points.front().position.domain
                == talas::project::ScoreTimeDomain::Seconds
            ? juce::String{"seconds"}
            : juce::String{"ticks"};
        using Transform = talas::app::AutomationTransform;
        juce::String title;
        juce::String firstLabel;
        double firstDefault = 0.0;
        juce::String secondLabel;
        double secondDefault = 0.0;
        Transform transform = Transform::Invert;
        switch (item)
        {
        case transformInvert:
            title = "Invert";
            firstLabel = "Axis value";
            firstDefault = 0.5;
            break;
        case transformLoop:
            transform = Transform::Loop;
            title = "Loop";
            firstLabel = "Repeat until (" + unit + ")";
            firstDefault = first + (last - first) * 4.0;
            break;
        case transformScale:
            transform = Transform::Scale;
            title = "Scale";
            firstLabel = "Factor";
            firstDefault = 1.0;
            break;
        case transformOffset:
            transform = Transform::Offset;
            title = "Offset";
            firstLabel = "Offset";
            break;
        case transformThreshold:
            transform = Transform::Threshold;
            title = "Threshold";
            firstLabel = "Threshold";
            firstDefault = 0.5;
            secondLabel = "Hysteresis";
            break;
        case transformTimeScale:
            transform = Transform::TimeScale;
            title = "Time Scale";
            firstLabel = "Factor";
            // The factor offered halves the function's duration.
            firstDefault = talas::app::automationTimeScaleOffered;
            break;
        case transformQuantize:
            transform = Transform::QuantizeEndpoints;
            title = "Quantize Endpoints";
            firstLabel = "Quantization (" + unit + ")";
            firstDefault = unit == "seconds" ? 0.25 : 240.0;
            break;
        case transformSpray:
            transform = Transform::Spray;
            title = "Spray";
            firstLabel = "Amount";
            firstDefault = 0.1;
            secondLabel = "Every (" + unit + ")";
            secondDefault = (last - first) > 0.0 ? (last - first) / 16.0 : 0.1;
            break;
        default:
            return;
        }
        // Pressed again, the transformation is taken away without asking.
        if (talas::app::automationLaneTransformed(*lane, transform))
        {
            (void) transformSelectedAutomationLane(transform, {});
            return;
        }
        auto* window = new juce::AlertWindow(
            title + " Automation", {}, juce::MessageBoxIconType::NoIcon, this);
        window->addTextEditor("first", juce::String(firstDefault), firstLabel);
        if (secondLabel.isNotEmpty())
            window->addTextEditor("second", juce::String(secondDefault), secondLabel);
        window->addButton("Apply", 1, juce::KeyPress{juce::KeyPress::returnKey});
        window->addButton("Cancel", 0, juce::KeyPress{juce::KeyPress::escapeKey});
        const auto laneId = lane->laneId;
        const auto selection = selection_;
        juce::Component::SafePointer<ScoreViewportComponent> safeThis(this);
        window->enterModalState(
            true,
            juce::ModalCallbackFunction::create(
                [safeThis, window, transform, laneId, selection, hasSecond = secondLabel.isNotEmpty()](int result) {
                    if (safeThis == nullptr || result != 1)
                        return;
                    const auto value = window->getTextEditorContents("first").getDoubleValue();
                    const auto secondary = hasSecond
                        ? window->getTextEditorContents("second").getDoubleValue()
                        : 0.0;
                    safeThis->selection_ = selection;
                    if (safeThis->selectedAutomationLane() == nullptr
                        || safeThis->selectedAutomationLane()->laneId != laneId)
                        return;
                    (void)safeThis->transformSelectedAutomationLane(
                        transform,
                        {value, secondary, static_cast<std::uint64_t>(juce::Time::getHighResolutionTicks())});
                }),
            true);
    }

    bool toggleSelectedAutomationInterpolation()
    {
        if (selection_.kind != TimelineScoreSelectionKind::AutomationPoint)
            return false;
        const auto& lanes = bridge_.editor().document().timeline.score.automation;
        const auto found = std::ranges::find(
            lanes, selection_.entityId, &talas::project::ScoreAutomationLane::laneId);
        if (found == lanes.end() || selection_.pointIndex >= found->points.size())
            return false;
        auto lane = *found;
        auto& interpolation = lane.points[selection_.pointIndex].interpolation;
        interpolation = interpolation == talas::core::AutomationInterpolation::Step
            ? talas::core::AutomationInterpolation::Linear
            : talas::core::AutomationInterpolation::Step;
        return commitDiscreteEdit(
            bridge_.editor().updateScoreAutomationLane(std::move(lane)),
            "Toggled Score automation curve");
    }

    bool adjustSelectedClipFade(bool fadeIn, int direction)
    {
        if (selection_.kind != TimelineScoreSelectionKind::Clip)
            return false;
        const auto& clips = bridge_.editor().document().timeline.score.clips;
        const auto found = std::ranges::find(
            clips, selection_.entityId, &talas::project::ScoreClip::clipId);
        if (found == clips.end())
            return false;
        auto clip = *found;
        auto& fade = fadeIn ? clip.fadeIn : clip.fadeOut;
        const auto& otherFade = fadeIn ? clip.fadeOut : clip.fadeIn;
        const auto amount = nudgeAmount(fade.domain, false);
        const auto adjusted = direction > 0
            ? addScoreTime(fade, amount)
            : subtractScoreTime(fade, amount);
        if (!adjusted.has_value())
            return true;
        const auto totalDuration = multiplyScoreTime(
            clip.duration, clip.repeatCount);
        if (!totalDuration.has_value())
            return true;
        const auto remaining = subtractScoreTime(*totalDuration, otherFade);
        if (!remaining.has_value())
            return true;
        fade = minimumScoreTime(*adjusted, *remaining);
        return commitDiscreteEdit(
            bridge_.editor().updateScoreClip(std::move(clip)),
            fadeIn ? "Updated Score clip fade-in"
                   : "Updated Score clip fade-out");
    }

public:
    [[nodiscard]] static juce::Colour submixColourAt(std::size_t index)
    {
        return juce::Colour::fromHSV(std::fmod(0.08F + 0.618034F * static_cast<float>(index), 1.0F), 0.62F, 0.95F, 1.0F);
    }

    [[nodiscard]] juce::Colour submixColour(std::string_view submixId) const
    {
        return submixColourAt(
            talas::app::timelineSubmixColourIndex(bridge_.editor().document().timeline.score, submixId).value_or(0));
    }

    // The Submix menu connects the track to a
    // submix or disconnects it, or makes a new one and connects to that.
    bool setTrackSend(std::string_view trackId, std::string_view submixId, bool connected)
    {
        std::string error;
        auto candidate = talas::app::timelineWithTrackSend(bridge_.editor().document(), trackId, submixId, connected, error);
        if (!candidate.has_value())
        {
            onStatus_(juceString(error));
            return false;
        }
        return commitDiscreteEdit(bridge_.editor().applyDocumentEdit(std::move(*candidate)),
            (connected ? "Connected the track to " : "Disconnected the track from ") + submixName(submixId));
    }

    // The selected lane's takes.
    bool commitLaneTake(std::optional<talas::project::ScoreAutomationLane> edited, juce::String status)
    {
        if (!edited.has_value())
        {
            onStatus_("The lane cannot take that take");
            return false;
        }
        return commitDiscreteEdit(bridge_.editor().updateScoreAutomationLane(std::move(*edited)), std::move(status));
    }

    // A Sound dropped into a controller's lane
    // gives its amplitude or frequency envelope, for as long as asked, as a
    // new take of the controller.
    void setEnvelopeExtractor(TimelinePanel::EnvelopeExtractor extractor)
    {
        envelopeExtractor_ = std::move(extractor);
    }

    bool extractEnvelopeIntoLane(std::string_view laneId, const talas::runtime::EnvelopeExtractionRequest& request)
    {
        const auto& lanes = bridge_.editor().document().timeline.score.automation;
        const auto lane = std::ranges::find(lanes, laneId, &talas::project::ScoreAutomationLane::laneId);
        if (lane == lanes.end() || !envelopeExtractor_)
            return false;
        if (!lane->points.empty() && lane->points.front().position.domain != talas::project::ScoreTimeDomain::Seconds)
        {
            onStatus_("An envelope goes into a control timed in seconds");
            return false;
        }
        const auto extracted = envelopeExtractor_(request);
        if (!extracted.ok())
        {
            onStatus_(juceString(extracted.error));
            return false;
        }
        auto points = talas::app::envelopeAutomationPoints(extracted.values, extracted.sampleRate, 0.02, 0.01);
        selection_ = {TimelineScoreSelectionKind::AutomationLane, std::string{laneId}, 0};
        return commitLaneTake(talas::app::automationLaneWithNewTake(*lane, std::move(points)),
            request.kind == talas::runtime::EnvelopeKind::Amplitude ? "Extracted the amplitude envelope"
                                                                 : "Extracted the frequency envelope");
    }

    // Record to disk..., each output channel checked
    // into a mono file of its own or two into an interleaved stereo file,
    // in the format and bit depth chosen, at the current sample rate.
    void setTimelineRecorder(TimelinePanel::TimelineRecorder recorder)
    {
        timelineRecorder_ = std::move(recorder);
    }

    [[nodiscard]] talas::audio::TimelineRecordingResult recordTimelineNow(talas::audio::TimelineRecordingOptions options)
    {
        if (!timelineRecorder_)
            return {{}, 0, "The Timeline cannot be recorded here", false};
        options.sampleRate = static_cast<double>(activeSampleRate());
        return timelineRecorder_(std::move(options))();
    }

    void recordTimeline(talas::audio::TimelineRecordingOptions options)
    {
        if (!timelineRecorder_)
            return;
        options.sampleRate = static_cast<double>(activeSampleRate());
        // A live recording ends with the Timeline, or
        // when its thermometer's Stop is pressed, keeping what it recorded.
        const auto live = options.live;
        auto stopped = std::make_shared<std::atomic<bool>>(false);
        options.shouldCancel = [cancelled = recordingCancelled_, stopped] { return cancelled->load() || stopped->load(); };
        // A thermometer shows the recording in
        // progress, each whole percent passed to the message thread.
        juce::Component::SafePointer<ScoreViewportComponent> safeThis(this);
        recordingProgress_ = 0.0;
        options.progress = [safeThis, shown = std::make_shared<std::atomic<int>>(-1)](double fraction) {
            const auto percent = static_cast<int>(fraction * 100.0);
            if (shown->exchange(percent) != percent)
                juce::MessageManager::callAsync([safeThis, fraction] {
                    if (safeThis != nullptr)
                        safeThis->recordingProgress_ = fraction;
                });
        };
        auto job = timelineRecorder_(std::move(options));
        if (!recordingPool_)
            recordingPool_ = std::make_unique<juce::ThreadPool>(1);
        if (!live)
            onStatus_("Recording the Timeline to disk...");
        if (isShowing())
        {
            recordingThermometer_ = std::make_unique<juce::AlertWindow>("Record to disk",
                live ? "Recording the Timeline as it plays. Start it with a MIDI key down if it waits for one."
                     : "Recording the Timeline",
                juce::MessageBoxIconType::NoIcon, this);
            recordingThermometer_->addProgressBarComponent(recordingProgress_);
            if (live)
            {
                // Not modal, so the surface's presets, keys and MIDI still
                // play the Timeline while it records.
                recordingStopButton_ = std::make_unique<juce::TextButton>("Stop recording");
                recordingStopButton_->setSize(160, 28);
                recordingStopButton_->onClick = [stopped] { stopped->store(true); };
                recordingThermometer_->addCustomComponent(recordingStopButton_.get());
                recordingThermometer_->setAlwaysOnTop(true);
                recordingThermometer_->setVisible(true);
                recordingThermometer_->toFront(false);
            }
            else
                recordingThermometer_->enterModalState(false);
        }
        recordingPool_->addJob([job = std::move(job), safeThis] {
            auto result = job();
            juce::MessageManager::callAsync([safeThis, result = std::move(result)] {
                if (safeThis == nullptr)
                    return;
                safeThis->recordingThermometer_.reset();
                safeThis->recordingStopButton_.reset();
                safeThis->onStatus_(result.ok()
                        ? "Recorded the Timeline into " + juce::String(static_cast<int>(result.files.size()))
                            + (result.files.size() == 1 ? " file" : " files")
                        : juceString(result.error));
            });
        });
    }

    void askToRecordTimeline()
    {
        const auto outputs = talas::audio::timelineRecordingChannelCount(bridge_.editor().document());
        if (outputs == 0)
        {
            onStatus_("The Timeline has no Master to record");
            return;
        }
        auto* window = new juce::AlertWindow("Record to disk",
            "Record what goes to each output channel checked, after the level and pan controls.",
            juce::MessageBoxIconType::NoIcon, this);
        window->addComboBox("format", {"WAV", "AIFF"}, "File format");
        window->addComboBox("bits", {"8 bits", "16 bits", "24 bits"}, "Bit depth");
        window->getComboBoxComponent("bits")->setSelectedItemIndex(1);
        auto toggles = std::make_shared<std::vector<std::unique_ptr<juce::ToggleButton>>>();
        for (std::uint32_t channel = 1; channel <= outputs; ++channel)
        {
            auto toggle = std::make_unique<juce::ToggleButton>("Output " + juce::String(static_cast<int>(channel)));
            toggle->setToggleState(true, juce::dontSendNotification);
            toggle->setSize(220, 24);
            window->addCustomComponent(toggle.get());
            toggles->push_back(std::move(toggle));
        }
        window->addComboBox("stereo", {"As an interleaved stereo file", "As two independent files"}, "Two channels");
        // Played live or rendered offline.
        window->addComboBox("mode",
            {juceString(talas::app::timelineRecordingModeLabel(talas::app::TimelineRecordingMode::Live)),
                juceString(talas::app::timelineRecordingModeLabel(talas::app::TimelineRecordingMode::Offline))},
            "Record");
        window->addButton("Record", 1, juce::KeyPress{juce::KeyPress::returnKey});
        window->addButton("Cancel", 0, juce::KeyPress{juce::KeyPress::escapeKey});
        juce::Component::SafePointer<ScoreViewportComponent> safeThis(this);
        window->enterModalState(true,
            juce::ModalCallbackFunction::create([safeThis, window, toggles](int result) {
                if (safeThis == nullptr || result != 1)
                    return;
                talas::audio::TimelineRecordingOptions options;
                for (std::size_t index = 0; index < toggles->size(); ++index)
                {
                    if ((*toggles)[index]->getToggleState())
                        options.channels.push_back(static_cast<std::uint32_t>(index + 1));
                }
                const auto aiff = window->getComboBoxComponent("format")->getSelectedItemIndex() == 1;
                options.format = aiff ? talas::audio::TimelineRecordingFormat::Aiff : talas::audio::TimelineRecordingFormat::Wav;
                const auto bits = window->getComboBoxComponent("bits")->getSelectedItemIndex();
                options.bits = bits == 0 ? 8 : bits == 2 ? 24 : 16;
                options.interleavedStereo = window->getComboBoxComponent("stereo")->getSelectedItemIndex() == 0;
                options.live = window->getComboBoxComponent("mode")->getSelectedItemIndex() == 0;
                safeThis->chooseRecordingDestination(std::move(options));
            }),
            true);
    }

    void chooseRecordingDestination(talas::audio::TimelineRecordingOptions options)
    {
        const auto extension = options.format == talas::audio::TimelineRecordingFormat::Aiff ? ".aiff" : ".wav";
        recordingChooser_ = std::make_unique<juce::FileChooser>("Record the Timeline to",
            juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile(juce::String("Timeline") + extension),
            juce::String("*") + extension);
        juce::Component::SafePointer<ScoreViewportComponent> safeThis(this);
        recordingChooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
            [safeThis, options = std::move(options), extension](const juce::FileChooser& chooser) mutable {
                if (safeThis == nullptr || chooser.getResult() == juce::File{})
                    return;
                options.destination = chooser.getResult().withFileExtension(extension).getFullPathName().toStdString();
                safeThis->recordTimeline(std::move(options));
            });
    }

    void askForEnvelope(std::string laneId, std::string definitionId)
    {
        const auto duration = talas::runtime::envelopeSoundDuration(bridge_.editor().document(), definitionId).value_or(5.0);
        auto* window = new juce::AlertWindow("Extract Envelope",
            "Extract the dropped Sound's amplitude or frequency envelope into this control.", juce::MessageBoxIconType::NoIcon, this);
        // Or the Sound's tempo for the Timeline's BPM.
        window->addComboBox("kind", {"Amplitude", "Frequency", "BPM"}, "Parameter to extract");
        window->addTextEditor("duration", juce::String(duration), "Duration (seconds)");
        window->addTextEditor("low", "50", "Lowest frequency to expect (hertz)");
        window->addTextEditor("high", "2000", "Highest frequency to expect (hertz)");
        window->addButton("OK", 1, juce::KeyPress{juce::KeyPress::returnKey});
        window->addButton("Cancel", 0, juce::KeyPress{juce::KeyPress::escapeKey});
        juce::Component::SafePointer<ScoreViewportComponent> safeThis(this);
        window->enterModalState(true,
            juce::ModalCallbackFunction::create([safeThis, window, laneId = std::move(laneId), definitionId = std::move(definitionId)](int result) {
                if (safeThis == nullptr || result != 1)
                    return;
                if (window->getComboBoxComponent("kind")->getSelectedItemIndex() == 2)
                {
                    safeThis->askForSoundTempo(definitionId);
                    return;
                }
                talas::runtime::EnvelopeExtractionRequest request;
                request.soundDefinitionId = definitionId;
                request.kind = window->getComboBoxComponent("kind")->getSelectedItemIndex() == 1 ? talas::runtime::EnvelopeKind::Frequency
                                                                                              : talas::runtime::EnvelopeKind::Amplitude;
                request.durationSeconds = window->getTextEditorContents("duration").getDoubleValue();
                request.minimumHz = window->getTextEditorContents("low").getDoubleValue();
                request.maximumHz = window->getTextEditorContents("high").getDoubleValue();
                request.sampleRate = static_cast<double>(safeThis->activeSampleRate());
                (void) safeThis->extractEnvelopeIntoLane(laneId, request);
            }),
            true);
    }

    // The time cursor's source menu.
    void setTimeSourceChooser(TimelinePanel::TimeSourceChooser chooser)
    {
        timeSourceChooser_ = std::move(chooser);
    }

    void setDragImporter(TimelinePanel::DragImporter importer)
    {
        dragImporter_ = std::move(importer);
    }

    void setSpaceKey(TimelinePanel::SpaceKey key)
    {
        spaceKey_ = std::move(key);
    }

    [[nodiscard]] unsigned timeCodeFramesPerSecond() const noexcept
    {
        switch (runtime_.smpteRate)
        {
        case talas::app::TimelineSmpteRate::Fps24: return 24;
        case talas::app::TimelineSmpteRate::Fps25: return 25;
        case talas::app::TimelineSmpteRate::Fps2997Drop:
        case talas::app::TimelineSmpteRate::Fps30: return 30;
        }
        return 30;
    }

    bool setTimeCodeOffset(std::string_view text)
    {
        std::string error;
        auto candidate = talas::app::timelineWithTimeCodeOffset(bridge_.editor().document(), text, timeCodeFramesPerSecond(), error);
        if (!candidate.has_value())
        {
            onStatus_(juceString(error));
            return false;
        }
        return commitDiscreteEdit(bridge_.editor().applyDocumentEdit(std::move(*candidate)),
            "The Timeline starts at time code " + juceString(std::string{text}));
    }

    void showTimeSourceMenu()
    {
        const auto& document = bridge_.editor().document();
        const auto label = talas::app::timelineTimeSourceLabel(document);
        juce::PopupMenu menu;
        menu.addItem(1, "Free running", static_cast<bool>(timeSourceChooser_), label == "Free running");
        menu.addItem(2, "Time code", static_cast<bool>(timeSourceChooser_), label == "Time code");
        menu.addSeparator();
        menu.addItem(3, "Set time code offset...");
        juce::Component::SafePointer<ScoreViewportComponent> safeThis(this);
        menu.showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(&timeSource_), [safeThis](int result) {
            if (safeThis == nullptr || result == 0)
                return;
            if (result == 3)
            {
                safeThis->askForTimeCodeOffset();
                return;
            }
            if (safeThis->timeSourceChooser_)
                (void) safeThis->timeSourceChooser_(result == 2);
        });
    }

    // The selected Sound's cache state as the Cache menu shows it.
    [[nodiscard]] talas::app::TimelineCacheLook cacheLookOf(std::string_view clipId)
    {
        if (!clipArtifactState_)
            return talas::app::TimelineCacheLook::Live;
        using State = TimelineScoreClipArtifactStateKind;
        switch (clipArtifactState_(clipId).kind)
        {
        case State::RenderingCache:
        case State::Freezing:
        case State::Cancelling:
        case State::CacheStale:
            return talas::app::TimelineCacheLook::Caching;
        case State::Cached:
        case State::Frozen:
            return talas::app::TimelineCacheLook::Cached;
        case State::Live:
        case State::Missing:
        case State::Invalid:
            break;
        }
        return talas::app::TimelineCacheLook::Live;
    }

    [[nodiscard]] std::optional<std::string> selectedClipId() const
    {
        return selection_.kind == TimelineScoreSelectionKind::Clip ? std::optional<std::string>{selection_.entityId}
                                                                   : std::nullopt;
    }

    void updateCacheMenuLook()
    {
        const auto clipId = selectedClipId();
        const auto look = clipId.has_value() ? cacheLookOf(*clipId) : talas::app::TimelineCacheLook::Live;
        cacheMenu_.setColour(juce::TextButton::buttonColourId, juce::Colour{talas::app::timelineCacheMenuColour(look)});
        cacheMenu_.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    }

    // The caches recorded from now on are in it.
    bool setTimelineCacheWordSize(unsigned bits)
    {
        const auto edited = talas::app::timelineWithCacheWordSize(bridge_.editor().document(), bits);
        if (!edited.has_value() || edited->timeline.cacheWordSize == bridge_.editor().document().timeline.cacheWordSize)
            return false;
        return commitInfoEdit(bridge_.editor().replaceDocument(*edited),
            "The Timeline caches as " + juceString(talas::app::timelineCacheFormatName(bits)));
    }

    // Recordings shared across the tracks, so a
    // reader and its MemoryWriter on separate tracks play together.
    bool setTimelineSharingAcrossTracks(bool shared)
    {
        if (bridge_.editor().document().timeline.sharingMemoryWritersAcrossTracks == shared)
            return false;
        return commitInfoEdit(bridge_.editor().replaceDocument(
                                  talas::app::timelineWithMemoryWritersSharedAcrossTracks(bridge_.editor().document(), shared)),
            shared ? "The Timeline's tracks share their recordings" : "Each track keeps its own recordings");
    }

    // Live, Cache and Re-cache for the selected Sound; Freeze and
    // Cancel as the clip's menu has them.
    bool setTimelineOutputs(std::uint32_t outputs)
    {
        const auto edited = talas::app::timelineWithOutputs(bridge_.editor().document(), outputs);
        if (!edited.has_value())
            return false;
        talas::project::ProjectScoreEditBatch batch;
        batch.tracks = edited->timeline.score.tracks;
        return commitInfoEdit(bridge_.editor().applyScoreEdits(std::move(batch)),
            "The Timeline has " + juce::String(static_cast<int>(outputs)) + (outputs == 1 ? " output" : " outputs"));
    }

    void showCacheMenu()
    {
        const auto clipId = selectedClipId();
        juce::PopupMenu menu;
        if (!clipId.has_value() || !clipArtifactState_ || !clipArtifactAction_)
        {
            menu.addItem(1, "Select a Sound to cache", false);
            menu.showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(&cacheMenu_));
            return;
        }
        const auto state = clipArtifactState_(*clipId);
        const auto items = talas::app::timelineCacheMenuItems(cacheLookOf(*clipId), state.sourceAvailable);
        for (std::size_t index = 0; index < items.size(); ++index)
            menu.addItem(static_cast<int>(index) + 1, juceString(items[index].name), items[index].enabled, items[index].ticked);
        menu.addSeparator();
        using State = TimelineScoreClipArtifactStateKind;
        const auto running = state.kind == State::RenderingCache || state.kind == State::Freezing || state.kind == State::Cancelling;
        menu.addItem(4, "Freeze", !running && state.kind != State::Frozen && state.sourceAvailable);
        menu.addItem(5, "Cancel", running);
        menu.addSectionHeader(juceString(state.detail.empty() ? state.label : state.detail));
        juce::Component::SafePointer<ScoreViewportComponent> safeThis(this);
        menu.showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(&cacheMenu_),
            [safeThis, clipId = *clipId, state](int result) {
                if (safeThis == nullptr || result == 0 || !safeThis->clipArtifactAction_)
                    return;
                using Action = TimelineScoreClipArtifactAction;
                const auto action = result == 1 ? Action::Live
                    : result == 2              ? Action::Cache
                    : result == 3              ? Action::RefreshCache
                    : result == 4              ? Action::Freeze
                                               : Action::Cancel;
                // Marked for caching, its live parameters
                // are armed for recording.
                if (action == Action::Cache)
                    (void) safeThis->armLiveLanesForCache(clipId);
                safeThis->clipArtifactAction_(clipId, action);
                safeThis->repaint();
            });
    }

    void askForTimeCodeOffset()
    {
        auto* window = new juce::AlertWindow("Set time code offset",
            "The starting time code of your video or other time code source.", juce::MessageBoxIconType::NoIcon, this);
        window->addTextEditor("offset",
            juceString(talas::app::timelineTimeCodeOffsetText(bridge_.editor().document(), timeCodeFramesPerSecond())),
            "Hours:minutes:seconds:frames");
        window->addButton("OK", 1, juce::KeyPress{juce::KeyPress::returnKey});
        window->addButton("Cancel", 0, juce::KeyPress{juce::KeyPress::escapeKey});
        juce::Component::SafePointer<ScoreViewportComponent> safeThis(this);
        window->enterModalState(true,
            juce::ModalCallbackFunction::create([safeThis, window](int result) {
                if (safeThis != nullptr && result == 1)
                    (void) safeThis->setTimeCodeOffset(window->getTextEditorContents("offset").toStdString());
            }),
            true);
    }

    bool setMasterBpm(double bpm)
    {
        std::string error;
        auto candidate = talas::app::timelineWithMasterBpm(bridge_.editor().document(), bpm, error);
        if (!candidate.has_value())
        {
            onStatus_(juceString(error));
            return false;
        }
        return commitDiscreteEdit(bridge_.editor().applyDocumentEdit(std::move(*candidate)),
            "The Timeline's master BPM is now " + juce::String(bpm, 2));
    }

    void askMasterBpm()
    {
        const auto& tempo = bridge_.editor().document().timeline.musicalTime.tempo;
        const auto current = tempo.empty() ? 120.0 : 60'000'000.0 / static_cast<double>(tempo.front().microsecondsPerQuarter);
        auto* window = new juce::AlertWindow("Set Master BPM", "The Timeline's one tempo, in beats per minute.",
            juce::MessageBoxIconType::NoIcon, this);
        window->addTextEditor("bpm", juce::String(current, 2), "BPM");
        window->addButton("OK", 1, juce::KeyPress{juce::KeyPress::returnKey});
        window->addButton("Cancel", 0, juce::KeyPress{juce::KeyPress::escapeKey});
        juce::Component::SafePointer<juce::Component> safeThis(this);
        window->enterModalState(true, juce::ModalCallbackFunction::create([this, safeThis, window](int result) {
            if (safeThis != nullptr && result == 1)
                (void) setMasterBpm(window->getTextEditorContents("bpm").getDoubleValue());
        }),
            true);
    }

    // The BPM inferred from a Sound's duration,
    // exactly one bar long or the beats heard in it.
    bool setTempoFromSound(std::string_view definitionId, std::optional<double> beats)
    {
        std::string error;
        auto candidate = talas::app::timelineWithSoundTempo(bridge_.editor().document(),
            talas::runtime::envelopeSoundDuration(bridge_.editor().document(), definitionId), beats, error);
        if (!candidate.has_value())
        {
            onStatus_(juceString(error));
            return false;
        }
        const auto bpm = 60'000'000.0 / static_cast<double>(candidate->timeline.musicalTime.tempo.front().microsecondsPerQuarter);
        return commitDiscreteEdit(bridge_.editor().applyDocumentEdit(std::move(*candidate)),
            "The Timeline's BPM is now " + juce::String(bpm, 2));
    }

    void askForSoundTempo(std::string definitionId)
    {
        auto* window = new juce::AlertWindow("BPM from a Sound",
            "Infer the Timeline's BPM from the Sound's duration.", juce::MessageBoxIconType::NoIcon, this);
        window->addComboBox("method",
            {"Assume this Sound is exactly one bar long", "Specify the duration of this Sound in beats"}, "Method");
        window->addTextEditor("beats", "4", "Beats you hear in this Sound");
        window->addButton("OK", 1, juce::KeyPress{juce::KeyPress::returnKey});
        window->addButton("Cancel", 0, juce::KeyPress{juce::KeyPress::escapeKey});
        juce::Component::SafePointer<ScoreViewportComponent> safeThis(this);
        window->enterModalState(true,
            juce::ModalCallbackFunction::create([safeThis, window, definitionId = std::move(definitionId)](int result) {
                if (safeThis == nullptr || result != 1)
                    return;
                const auto beats = window->getComboBoxComponent("method")->getSelectedItemIndex() == 1
                    ? std::optional<double>{window->getTextEditorContents("beats").getDoubleValue()}
                    : std::nullopt;
                (void) safeThis->setTempoFromSound(definitionId, beats);
            }),
            true);
    }

    // The selected lane slaved to a master, or
    // freed; and New Master.
    bool slaveSelectedLane(const std::string& masterLaneId)
    {
        const auto* lane = selectedAutomationLane();
        if (lane == nullptr)
            return false;
        const auto status = masterLaneId.empty() ? juce::String{"The control is its own again"}
                                                 : "The control is slaved to " + juceString(masterLaneId);
        return commitLaneTake(talas::app::automationLaneSlavedTo(bridge_.editor().document().timeline.score, lane->laneId, masterLaneId),
            status);
    }

    bool promoteSelectedLaneToMaster()
    {
        const auto* lane = selectedAutomationLane();
        if (lane == nullptr)
            return false;
        auto promoted = talas::app::automationLanePromotedToMaster(
            bridge_.editor().document().timeline.score, lane->laneId, uniqueScoreId("master"));
        if (!promoted.has_value())
        {
            onStatus_("Only a control that is its own and has no slaves can be promoted to master");
            return false;
        }
        const auto name = promoted->first.eventName;
        talas::project::ProjectScoreEditBatch batch;
        batch.automation = {std::move(promoted->first), std::move(promoted->second)};
        return commitDiscreteEdit(bridge_.editor().applyScoreEdits(std::move(batch)),
            "Promoted the control to the master control " + juceString(name));
    }

    bool addMasterControl(const std::string& name)
    {
        auto lane = talas::app::newMasterControl(bridge_.editor().document().timeline.score, name, uniqueScoreId("master"));
        if (!lane.has_value())
        {
            onStatus_("A master control needs a new name of letters, digits and underscores");
            return false;
        }
        talas::project::ProjectScoreEditBatch batch;
        const auto laneId = lane->laneId;
        batch.automation.push_back(std::move(*lane));
        const auto result = bridge_.editor().applyScoreEdits(std::move(batch));
        if (result.ok())
            selection_ = {TimelineScoreSelectionKind::AutomationLane, laneId, 0};
        return commitDiscreteEdit(result, "New master control " + juceString(name));
    }

    void askForNewMasterControl()
    {
        auto* window = new juce::AlertWindow("New Master", "Name the new master control.", juce::MessageBoxIconType::NoIcon, this);
        window->addTextEditor("name", "Master");
        window->addButton("OK", 1, juce::KeyPress{juce::KeyPress::returnKey});
        window->addButton("Cancel", 0, juce::KeyPress{juce::KeyPress::escapeKey});
        juce::Component::SafePointer<ScoreViewportComponent> safeThis(this);
        window->enterModalState(true,
            juce::ModalCallbackFunction::create([safeThis, window](int result) {
                if (safeThis != nullptr && result == 1)
                    (void) safeThis->addMasterControl(window->getTextEditorContents("name").trim().toStdString());
            }),
            true);
    }

    bool newSelectedLaneTake()
    {
        const auto* lane = selectedAutomationLane();
        return lane != nullptr
            && commitLaneTake(talas::app::automationLaneWithNewTake(*lane), "New take " + juceString(talas::app::nextAutomationTakeName(*lane)));
    }

    bool chooseSelectedLaneTake(const std::string& name)
    {
        const auto* lane = selectedAutomationLane();
        return lane != nullptr && commitLaneTake(talas::app::automationLaneWithTake(*lane, name), "Source of control: " + juceString(name));
    }

    bool renameSelectedLaneTake(const std::string& name)
    {
        const auto* lane = selectedAutomationLane();
        return lane != nullptr
            && commitLaneTake(talas::app::automationLaneWithTakeRenamed(*lane, lane->takeName, name), "Renamed the take " + juceString(name));
    }

    // A live lane's MIDI channel and controller,
    // chosen into the project's global map.
    void askSelectedLaneControlSource()
    {
        const auto* lane = selectedAutomationLane();
        if (lane == nullptr)
            return;
        const auto entries = talas::project::projectGlobalMapEntries(bridge_.editor().document());
        const auto entry = std::ranges::find(entries, lane->eventName, &talas::project::GlobalMapEntry::eventName);
        auto* window = new juce::AlertWindow("Source of Control", "The MIDI channel and controller of " + juceString(lane->eventName),
            juce::MessageBoxIconType::NoIcon, this);
        juce::StringArray channels;
        for (auto channel = 1; channel <= 16; ++channel)
            channels.add(juce::String(channel));
        window->addComboBox("channel", channels, "MIDI channel");
        window->getComboBoxComponent("channel")->setSelectedItemIndex(
            entry == entries.end() ? 0 : entry->channel.value_or(1) - 1, juce::dontSendNotification);
        window->addTextEditor("controller",
            juce::String(entry == entries.end() || !entry->source.empty() ? 7 : entry->controller), "Controller (0 to 127)");
        window->addButton("OK", 1, juce::KeyPress{juce::KeyPress::returnKey});
        window->addButton("Cancel", 0, juce::KeyPress{juce::KeyPress::escapeKey});
        juce::Component::SafePointer<ScoreViewportComponent> safeThis(this);
        window->enterModalState(true,
            juce::ModalCallbackFunction::create([safeThis, window, name = lane->eventName](int result) {
                if (safeThis == nullptr || result != 1)
                    return;
                const auto controller = window->getTextEditorContents("controller").trim().getIntValue();
                (void) safeThis->setLaneControlSource(name,
                    static_cast<std::uint8_t>(window->getComboBoxComponent("channel")->getSelectedItemIndex() + 1),
                    static_cast<std::uint8_t>(std::clamp(controller, 0, 127)));
            }),
            true);
    }

    bool setLaneControlSource(const std::string& eventName, std::uint8_t channel, std::uint8_t controller)
    {
        const auto& document = bridge_.editor().document();
        const auto map = talas::app::globalMapWithControlSource(
            document.globalMap.empty() ? talas::project::defaultGlobalMapText() : document.globalMap, eventName, channel, controller);
        if (!map.has_value())
        {
            onStatus_("That MIDI channel and controller cannot be the lane's");
            return false;
        }
        return commitDiscreteEdit(bridge_.editor().setGlobalMap(*map),
            juceString(eventName) + " follows MIDI channel " + juce::String(channel) + ", controller " + juce::String(controller));
    }

    void askToRenameSelectedLaneTake()
    {
        const auto* lane = selectedAutomationLane();
        if (lane == nullptr)
            return;
        auto* window = new juce::AlertWindow("Rename Take", "Give the take a more meaningful name.", juce::MessageBoxIconType::NoIcon, this);
        window->addTextEditor("name", juceString(lane->takeName));
        window->addButton("OK", 1, juce::KeyPress{juce::KeyPress::returnKey});
        window->addButton("Cancel", 0, juce::KeyPress{juce::KeyPress::escapeKey});
        juce::Component::SafePointer<ScoreViewportComponent> safeThis(this);
        window->enterModalState(true,
            juce::ModalCallbackFunction::create([safeThis, window](int result) {
                if (safeThis != nullptr && result == 1)
                    (void) safeThis->renameSelectedLaneTake(window->getTextEditorContents("name").trim().toStdString());
            }),
            true);
    }

    // An output channel toggled in the audio output menu, or 0
    // for the pan controls.
    bool toggleTrackOutput(std::string_view trackId, std::uint32_t channel)
    {
        const auto& tracks = bridge_.editor().document().timeline.score.tracks;
        const auto track = std::ranges::find(tracks, trackId, &talas::project::ScoreTrack::trackId);
        if (track == tracks.end())
            return false;
        auto channels = track->outputChannels;
        if (channel == 0)
            channels.clear();
        else if (const auto found = std::ranges::find(channels, channel); found != channels.end())
            channels.erase(found);
        else
            channels.push_back(channel);
        std::string error;
        auto candidate = talas::app::timelineWithTrackOutputs(bridge_.editor().document(), trackId, std::move(channels), error);
        if (!candidate.has_value())
        {
            onStatus_(juceString(error));
            return false;
        }
        return commitDiscreteEdit(bridge_.editor().applyDocumentEdit(std::move(*candidate)),
            channel == 0 ? juce::String{"The track uses its pan controls"} : juce::String{"Changed the track's output channels"});
    }

    // No output.
    bool setTrackNoOutput(std::string_view trackId)
    {
        std::string error;
        auto candidate = talas::app::timelineWithTrackNoOutput(bridge_.editor().document(), trackId, error);
        if (!candidate.has_value())
        {
            onStatus_(juceString(error));
            return false;
        }
        return commitDiscreteEdit(bridge_.editor().applyDocumentEdit(std::move(*candidate)),
            "The track is heard only through its sends");
    }

    // The MIDI status control's menu, the
    // channel on which the track's Sounds expect MIDI.
    bool setTrackMidiChannel(std::string_view trackId, std::uint8_t channel)
    {
        std::string error;
        auto candidate = talas::app::timelineWithTrackMidiChannel(bridge_.editor().document(), trackId, channel, error);
        if (!candidate.has_value())
        {
            onStatus_(juceString(error));
            return false;
        }
        return commitDiscreteEdit(bridge_.editor().applyDocumentEdit(std::move(*candidate)),
            channel == 0 ? juce::String{"The track's Sounds take MIDI on the default channel"}
                         : "The track's Sounds take MIDI on channel " + juce::String(static_cast<int>(channel)));
    }

    // The input status of the Sound under the time cursor in the
    // track.
    [[nodiscard]] talas::app::TimelineInputStatus trackInputStatus(std::size_t row) const
    {
        for (const auto& item : viewport_.items)
        {
            if (item.kind == TimelineScoreItemKind::Clip && item.row == row && item.startFrame <= runtime_.playheadFrame
                && runtime_.playheadFrame < item.endFrame)
                return talas::app::timelineClipInputStatus(bridge_.editor().document(), item.stableId);
        }
        return {};
    }

    bool addSubmixForTrack(std::string_view trackId, std::string_view name)
    {
        std::string error;
        auto candidate = talas::app::timelineWithNewSubmix(bridge_.editor().document(), trackId, name, error);
        if (!candidate.has_value())
        {
            onStatus_(juceString(error));
            return false;
        }
        return commitDiscreteEdit(bridge_.editor().applyDocumentEdit(std::move(*candidate)),
            "Connected the track to a new submix, " + juceString(std::string{name}));
    }

    void askForNewSubmix(std::string trackId)
    {
        auto* window = new juce::AlertWindow("New Submix", "Name the new submix.", juce::MessageBoxIconType::NoIcon, this);
        window->addTextEditor("name", juceString(talas::app::timelineNewSubmixName(
            bridge_.editor().document().timeline.score, static_cast<std::uint64_t>(juce::Random::getSystemRandom().nextInt(26)))));
        window->addButton("OK", 1, juce::KeyPress{juce::KeyPress::returnKey});
        window->addButton("Cancel", 0, juce::KeyPress{juce::KeyPress::escapeKey});
        juce::Component::SafePointer<ScoreViewportComponent> safeThis(this);
        window->enterModalState(true,
            juce::ModalCallbackFunction::create([safeThis, window, trackId = std::move(trackId)](int result) {
                if (safeThis != nullptr && result == 1)
                    (void) safeThis->addSubmixForTrack(trackId, window->getTextEditorContents("name").trim().toStdString());
            }),
            true);
    }

    [[nodiscard]] juce::String submixName(std::string_view submixId) const
    {
        const auto& tracks = bridge_.editor().document().timeline.score.tracks;
        const auto found = std::ranges::find(tracks, submixId, &talas::project::ScoreTrack::trackId);
        return juceString(found == tracks.end() || found->displayName.empty() ? std::string{submixId} : found->displayName);
    }

    // The track input source control's choice.
    bool setTrackInput(std::string_view trackId, std::string_view inputTrackId)
    {
        std::string error;
        auto candidate = talas::app::timelineWithTrackInput(bridge_.editor().document(), trackId, inputTrackId, error);
        if (!candidate.has_value())
        {
            onStatus_(juceString(error));
            return false;
        }
        return commitDiscreteEdit(bridge_.editor().applyDocumentEdit(std::move(*candidate)),
            inputTrackId.empty() ? "The track has no input" : "The track takes " + submixName(inputTrackId) + " as its input");
    }

    [[nodiscard]] std::vector<juce::String> trackInputMenuForSmoke(std::string_view trackId) const
    {
        std::vector<juce::String> labels{"No input"};
        for (const auto& choice : talas::app::timelineTrackInputChoices(bridge_.editor().document().timeline.score, trackId))
            labels.push_back(juceString(choice.displayName));
        for (std::uint32_t channel = 1; channel <= talas::project::maximumScoreInputChannels; ++channel)
            labels.push_back("Input: " + juce::String(static_cast<int>(channel)));
        return labels;
    }

    // One of the hardware's audio inputs as the
    // track's input.
    bool setTrackHardwareInput(std::string_view trackId, std::uint32_t channel)
    {
        std::string error;
        auto candidate = talas::app::timelineWithTrackHardwareInput(bridge_.editor().document(), trackId, channel, error);
        if (!candidate.has_value())
        {
            onStatus_(juceString(error));
            return false;
        }
        return commitDiscreteEdit(bridge_.editor().applyDocumentEdit(std::move(*candidate)),
            "The track takes Input: " + juce::String(static_cast<int>(channel)) + " as its input");
    }

private:
    bool routeSelectedTrack(std::string outputTrackId)
    {
        if (!timelineScoreHasCommand(
                viewport_.commands, TimelineScoreCommand::ChangeOutput))
            return false;
        if (selection_.kind != TimelineScoreSelectionKind::Track)
            return false;
        const auto& tracks = bridge_.editor().document().timeline.score.tracks;
        const auto found = std::ranges::find(
            tracks, selection_.entityId, &talas::project::ScoreTrack::trackId);
        if (found == tracks.end())
            return false;
        auto track = *found;
        track.outputTrackId = std::move(outputTrackId);
        return commitDiscreteEdit(
            bridge_.editor().updateScoreTrack(std::move(track)),
            "Updated exact Score track route");
    }

    [[nodiscard]] static juce::String controllerRecordingTerminalText(
        talas::project::ControllerRecordingTerminal terminal)
    {
        switch (terminal)
        {
        case talas::project::ControllerRecordingTerminal::Completed:
            return "completed";
        case talas::project::ControllerRecordingTerminal::PunchComplete:
            return "punch complete";
        case talas::project::ControllerRecordingTerminal::Cancelled:
            return "cancelled";
        case talas::project::ControllerRecordingTerminal::SourceLost:
            return "source lost";
        case talas::project::ControllerRecordingTerminal::DeviceChanged:
            return "device changed";
        case talas::project::ControllerRecordingTerminal::Seeked:
            return "seeked";
        case talas::project::ControllerRecordingTerminal::StageChanged:
            return "stage changed";
        case talas::project::ControllerRecordingTerminal::CapacityExceeded:
            return "capacity exceeded";
        }
        return "incomplete";
    }

    void refreshControllerRecordingState()
    {
        const auto collected = bridge_.collectControllerRecordingTake();
        if (!collected.has_value())
            return;
        armedControllerRecordingTake_.reset();
        controllerRecordingPreview_.reset();
        if (discardControllerRecordingOnCollection_)
        {
            discardControllerRecordingOnCollection_ = false;
            controllerRecordingTake_.reset();
            onStatus_("Discarded raw controller recording without editing the Score");
            return;
        }
        controllerRecordingTake_ = *collected;
        onStatus_(
            "Controller recording "
            + controllerRecordingTerminalText(collected->terminal)
            + juceString(" · raw events ")
            + juce::String(static_cast<juce::int64>(
                collected->events.size())));
    }

    [[nodiscard]] const talas::project::ScoreAutomationLane*
    selectedAutomationLane() const
    {
        if (selection_.kind != TimelineScoreSelectionKind::AutomationLane
            && selection_.kind != TimelineScoreSelectionKind::AutomationPoint)
        {
            return nullptr;
        }
        const auto& lanes = bridge_.editor().document().timeline.score.automation;
        const auto found = std::ranges::find(
            lanes, selection_.entityId,
            &talas::project::ScoreAutomationLane::laneId);
        return found == lanes.end() ? nullptr : &*found;
    }

    [[nodiscard]] bool canArmControllerRecording(
        const talas::project::ScoreAutomationLane& lane) const
    {
        if (!acceptedStageCurrent_()
            || bridge_.latestStageGeneration() == 0
            || !bridge_.realtimeEventValueIdForLatestStage(
                lane.eventName,
                lane.domain).has_value())
        {
            return false;
        }
        const auto& score = bridge_.editor().document().timeline.score;
        if (lane.scope != talas::project::ScoreAutomationScope::Clip)
        {
            return std::ranges::find(
                       score.tracks,
                       lane.ownerId,
                       &talas::project::ScoreTrack::trackId)
                != score.tracks.end();
        }
        const auto clip = std::ranges::find(
            score.clips,
            lane.ownerId,
            &talas::project::ScoreClip::clipId);
        if (clip == score.clips.end())
            return false;
        const auto repeated = multiplyScoreTime(
            clip->duration,
            clip->repeatCount);
        const auto endTime = repeated.has_value()
            ? addScoreTime(clip->start, *repeated)
            : std::optional<talas::project::ScoreTime>{};
        const auto startFrame = talas::project::scoreTimeToSampleFrame(
            clip->start,
            bridge_.editor().document().timeline.musicalTime,
            runtime_.sampleRate);
        const auto endFrame = endTime.has_value()
            ? talas::project::scoreTimeToSampleFrame(
                *endTime,
                bridge_.editor().document().timeline.musicalTime,
                runtime_.sampleRate)
            : std::optional<std::uint64_t>{};
        return startFrame.has_value() && endFrame.has_value()
            && *endFrame > *startFrame
            && std::ranges::find(
                   score.tracks,
                   clip->trackId,
                   &talas::project::ScoreTrack::trackId)
                != score.tracks.end();
    }

    struct PendingRecordingTarget
    {
        talas::project::ControllerRecordingTarget target;
        std::optional<std::uint64_t> punchStartFrame;
        std::optional<std::uint64_t> punchEndFrame;
    };

    // What a recording takes: the lanes
    // Record-enabled, or else the one selected.
    [[nodiscard]] std::vector<const talas::project::ScoreAutomationLane*> recordingLanes() const
    {
        auto lanes = talas::app::recordEnabledLanes(bridge_.editor().document().timeline.score);
        if (lanes.empty())
        {
            if (const auto* lane = selectedAutomationLane())
                lanes.push_back(lane);
        }
        return lanes;
    }

    [[nodiscard]] std::optional<PendingRecordingTarget> controllerRecordingTargetFor(
        const talas::project::ScoreAutomationLane& lane)
    {
        const auto eventId = bridge_.realtimeEventValueIdForLatestStage(
            lane.eventName, lane.domain);
        if (!eventId.has_value())
        {
            onStatus_("Selected automation target is unavailable in the accepted stage");
            return std::nullopt;
        }
        std::uint64_t ownerStartFrame = 0;
        std::uint64_t ownerStartTick = 0;
        std::optional<std::uint64_t> punchStartFrame;
        std::optional<std::uint64_t> punchEndFrame;
        const auto& score = bridge_.editor().document().timeline.score;
        const talas::project::ScoreTrack* ownerTrack = nullptr;
        if (lane.scope == talas::project::ScoreAutomationScope::Clip)
        {
            const auto& clips = score.clips;
            const auto clip = std::ranges::find(
                clips, lane.ownerId, &talas::project::ScoreClip::clipId);
            const auto start = clip == clips.end()
                ? std::optional<std::uint64_t>{}
                : talas::project::scoreTimeToSampleFrame(
                    clip->start,
                    bridge_.editor().document().timeline.musicalTime,
                    runtime_.sampleRate);
            if (!start.has_value())
            {
                onStatus_("Clip-local recording owner cannot be represented at the active sample rate");
                return std::nullopt;
            }
            ownerStartFrame = *start;
            const auto repeated = multiplyScoreTime(
                clip->duration,
                clip->repeatCount);
            const auto endTime = repeated.has_value()
                ? addScoreTime(clip->start, *repeated)
                : std::optional<talas::project::ScoreTime>{};
            const auto end = endTime.has_value()
                ? talas::project::scoreTimeToSampleFrame(
                    *endTime,
                    bridge_.editor().document().timeline.musicalTime,
                    runtime_.sampleRate)
                : std::optional<std::uint64_t>{};
            if (!end.has_value() || *end <= ownerStartFrame)
            {
                onStatus_("Clip-local recording range cannot be represented at the active sample rate");
                return std::nullopt;
            }
            punchStartFrame = ownerStartFrame;
            punchEndFrame = *end;
            if (clip->start.domain == talas::project::ScoreTimeDomain::MusicalTicks)
                ownerStartTick = clip->start.tick;
            const auto track = clip == clips.end()
                ? score.tracks.end()
                : std::ranges::find(
                    score.tracks,
                    clip->trackId,
                    &talas::project::ScoreTrack::trackId);
            if (track != score.tracks.end())
                ownerTrack = &*track;
        }
        else
        {
            const auto track = std::ranges::find(
                score.tracks,
                lane.ownerId,
                &talas::project::ScoreTrack::trackId);
            if (track != score.tracks.end())
                ownerTrack = &*track;
        }
        if (ownerTrack == nullptr)
        {
            onStatus_("Controller recording owner is no longer available");
            return std::nullopt;
        }
        return PendingRecordingTarget{
            {
                *eventId,
                lane.laneId,
                lane.scope,
                lane.ownerId,
                lane.eventName,
                lane.domain,
                ownerTrack->timeDomain,
                ownerStartFrame,
                ownerStartTick,
            },
            punchStartFrame,
            punchEndFrame,
        };
    }

    bool toggleControllerRecording()
    {
        finishOwnedGesture();
        refreshControllerRecordingState();
        if (armedControllerRecordingTake_.has_value())
        {
            bridge_.stopControllerRecording();
            onStatus_("Stopping controller recording at the next audio boundary");
            return true;
        }
        if (controllerRecordingTake_.has_value())
        {
            onStatus_("Preview, approve, or discard the retained raw take before recording again");
            return true;
        }
        const auto lanes = recordingLanes();
        if (lanes.empty())
        {
            onStatus_("Select one exact automation lane, or record-enable lanes, before recording");
            return selection_.kind != TimelineScoreSelectionKind::None;
        }
        if (!acceptedStageCurrent_())
        {
            onStatus_("Stage the current project before recording controller automation");
            return true;
        }
        std::vector<talas::project::ControllerRecordingTarget> targets;
        std::optional<std::uint64_t> punchStartFrame;
        std::optional<std::uint64_t> punchEndFrame;
        for (const auto* lane : lanes)
        {
            auto target = controllerRecordingTargetFor(*lane);
            if (!target.has_value())
                return true;
            if (std::ranges::find(targets, target->target.eventId, &talas::project::ControllerRecordingTarget::eventId)
                != targets.end())
            {
                onStatus_("Two record-enabled lanes record the same control");
                return true;
            }
            // Several clips' lanes record over the time all of them span.
            if (target->punchStartFrame.has_value())
            {
                punchStartFrame = std::max(punchStartFrame.value_or(0), *target->punchStartFrame);
                punchEndFrame = punchEndFrame.has_value()
                    ? std::min(*punchEndFrame, *target->punchEndFrame)
                    : *target->punchEndFrame;
            }
            targets.push_back(target->target);
        }
        if (punchStartFrame.has_value() && *punchEndFrame <= *punchStartFrame)
        {
            onStatus_("The record-enabled lanes' clips do not overlap in time");
            return true;
        }
        const auto armed = bridge_.beginControllerRecording(
            targets,
            punchStartFrame,
            punchEndFrame);
        if (!armed.accepted)
        {
            onStatus_(juceString(armed.message));
            return true;
        }
        armedControllerRecordingTake_ = armed.takeGeneration;
        discardControllerRecordingOnCollection_ = false;
        onStatus_(
            "Recording exact controller events into raw take "
            + juce::String(static_cast<juce::int64>(armed.takeGeneration)));
        return true;
    }

    bool previewControllerRecording()
    {
        refreshControllerRecordingState();
        if (!controllerRecordingTake_.has_value())
        {
            onStatus_("Stop a complete controller recording before previewing simplification");
            return true;
        }
        if (!controllerRecordingTakeAuthorityCurrent())
        {
            onStatus_("Controller recording preview cancelled because its stage or lane authority changed");
            return true;
        }
        if (nextControllerRecordingPreviewGeneration_
            == std::numeric_limits<std::uint64_t>::max())
        {
            onStatus_("Controller recording preview generation space is exhausted");
            return true;
        }
        constexpr double tolerance = 0.01;
        const auto preview =
            talas::project::previewControllerRecordingSimplification(
                *controllerRecordingTake_,
                tolerance,
                nextControllerRecordingPreviewGeneration_++);
        if (!preview.ok())
        {
            onStatus_(preview.error.has_value()
                ? juceString(preview.error->message)
                : juce::String{"Controller recording preview failed"});
            return true;
        }
        controllerRecordingPreview_ = *preview.preview;
        onStatus_(
            juceString("Preview only · raw ")
            + juce::String(static_cast<juce::int64>(
                controllerRecordingPreview_->rawEventCount))
            + juceString(" · retained ")
            + juce::String(static_cast<juce::int64>(
                controllerRecordingPreview_->events.size()))
            + juceString(" · tolerance 0.01 · press Return to approve"));
        return true;
    }

    [[nodiscard]] bool controllerRecordingTakeAuthorityCurrent() const
    {
        if (!acceptedStageCurrent_()
            || !controllerRecordingTake_.has_value()
            || bridge_.latestStageGeneration()
                != controllerRecordingTake_->stageGeneration)
        {
            return false;
        }
        return takeTargetsMatchRecordingLanes(*controllerRecordingTake_);
    }

    // A take stays the recording lanes' own while they are the lanes it was
    // recorded into, each still the same control.
    [[nodiscard]] bool takeTargetsMatchRecordingLanes(const talas::project::ControllerRecordingTake& take) const
    {
        const auto lanes = recordingLanes();
        if (lanes.empty() || lanes.size() != take.targets.size())
            return false;
        return std::ranges::all_of(lanes, [&take](const talas::project::ScoreAutomationLane* lane) {
            const auto target = std::ranges::find(
                take.targets, lane->laneId, &talas::project::ControllerRecordingTarget::laneId);
            return target != take.targets.end()
                && lane->scope == target->scope
                && lane->ownerId == target->ownerId
                && lane->eventName == target->eventName
                && lane->domain == target->domain;
        });
    }

    [[nodiscard]] bool controllerRecordingApprovalAuthorityCurrent() const
    {
        return controllerRecordingPreview_.has_value()
            && controllerRecordingTakeAuthorityCurrent();
    }

    bool approveControllerRecording()
    {
        refreshControllerRecordingState();
        if (!controllerRecordingTake_.has_value()
            || !controllerRecordingPreview_.has_value())
        {
            onStatus_("Create an explicit controller recording preview before approval");
            return true;
        }
        if (!acceptedStageCurrent_()
            || bridge_.latestStageGeneration()
                != controllerRecordingTake_->stageGeneration)
        {
            onStatus_("Controller recording approval cancelled because the stage changed");
            return true;
        }
        if (!takeTargetsMatchRecordingLanes(*controllerRecordingTake_))
        {
            onStatus_("Controller recording approval cancelled because its lane authority changed");
            return true;
        }
        auto approved = talas::project::approveControllerRecordingSimplification(
            *controllerRecordingTake_,
            *controllerRecordingPreview_);
        if (!approved.ok())
        {
            onStatus_(approved.error.has_value()
                ? juceString(approved.error->message)
                : juce::String{"Controller recording approval failed"});
            return true;
        }
        // The approved lanes come back Automated, playing what was recorded
        // into them, each a new take beside the
        // function it had.
        for (auto& recorded : approved.edits->automation)
        {
            const auto& lanes = bridge_.editor().document().timeline.score.automation;
            const auto existing = std::ranges::find(lanes, recorded.laneId, &talas::project::ScoreAutomationLane::laneId);
            if (existing == lanes.end() || existing->points.empty())
                continue;
            if (auto taken = talas::app::automationLaneWithNewTake(*existing, recorded.points))
            {
                taken->state = recorded.state;
                recorded = std::move(*taken);
            }
        }
        const auto result = bridge_.editor().applyScoreEdits(
            std::move(*approved.edits));
        if (!result.ok())
        {
            onStatus_(juceString(result.message));
            return true;
        }
        controllerRecordingTake_.reset();
        controllerRecordingPreview_.reset();
        onDocumentChange_();
        rebuild(bridge_.editor().document(), runtime_);
        onStatus_("Approved controller recording simplification as one Score edit");
        return true;
    }

    bool discardControllerRecording()
    {
        if (armedControllerRecordingTake_.has_value())
        {
            discardControllerRecordingOnCollection_ = true;
            bridge_.stopControllerRecording(
                talas::project::ControllerRecordingTerminal::Cancelled);
            onStatus_("Cancelling controller recording at the next audio boundary");
            return true;
        }
        controllerRecordingTake_.reset();
        controllerRecordingPreview_.reset();
        onStatus_("Discarded raw controller recording without editing the Score");
        return true;
    }

    [[nodiscard]] TimelineControllerRecordingMenuState
    controllerRecordingMenuState() const
    {
        const auto lanes = recordingLanes();
        const auto canArm = !lanes.empty()
            && std::ranges::all_of(lanes, [this](const talas::project::ScoreAutomationLane* lane) {
                   return canArmControllerRecording(*lane);
               })
            && !armedControllerRecordingTake_.has_value()
            && !controllerRecordingTake_.has_value();
        const auto canPreview = controllerRecordingTakeAuthorityCurrent()
            && talas::project::controllerRecordingTakeAllowsApproval(
                *controllerRecordingTake_);
        return {
            armedControllerRecordingTake_.has_value() || canArm,
            canPreview,
            controllerRecordingApprovalAuthorityCurrent(),
            armedControllerRecordingTake_.has_value()
                || controllerRecordingTake_.has_value(),
        };
    }

    bool performControllerRecordingMenuAction(int actionId)
    {
        const auto state = controllerRecordingMenuState();
        if (actionId == 40 && state.startOrStopEnabled)
            return toggleControllerRecording();
        if (actionId == 41 && state.previewEnabled)
            return previewControllerRecording();
        if (actionId == 42 && state.approveEnabled)
            return approveControllerRecording();
        if (actionId == 43 && state.discardEnabled)
            return discardControllerRecording();
        return false;
    }

    void showContextMenu()
    {
        refreshControllerRecordingState();
        juce::PopupMenu menu;
        std::vector<std::string> routeIds;
        std::vector<std::string> inputIds;
        std::vector<std::string> takeNames;
        std::vector<std::string> masterIds;
        std::vector<std::pair<std::string, bool>> submixToggles;
        std::vector<std::string> clipDefinitionIds;
        const auto commandSelection = selection_;
        // With nothing selected, New Master.
        if (commandSelection.kind == TimelineScoreSelectionKind::None)
        {
            menu.addItem(takeMenuBase - 3, "New Master Control...");
            menu.addItem(takeMenuBase - 5, "Make a Sound of the Timeline");
        }
        const auto commandFocusIdentity = selectedFocusIdentity_;
        const auto& score = bridge_.editor().document().timeline.score;
        std::optional<talas::project::ScoreTrack> commandTrack;
        std::optional<talas::project::ScoreClip> commandClip;
        std::optional<talas::project::ScoreAutomationLane> commandLane;
        std::optional<talas::project::ScoreMarker> commandMarker;
        if (commandSelection.kind == TimelineScoreSelectionKind::Track)
        {
            const auto found = std::ranges::find(
                score.tracks, commandSelection.entityId,
                &talas::project::ScoreTrack::trackId);
            if (found != score.tracks.end())
                commandTrack = *found;
        }
        else if (commandSelection.kind == TimelineScoreSelectionKind::Clip)
        {
            const auto found = std::ranges::find(
                score.clips, commandSelection.entityId,
                &talas::project::ScoreClip::clipId);
            if (found != score.clips.end())
                commandClip = *found;
        }
        else if (commandSelection.kind
            == TimelineScoreSelectionKind::AutomationLane
            || commandSelection.kind
                == TimelineScoreSelectionKind::AutomationPoint)
        {
            const auto found = std::ranges::find(
                score.automation, commandSelection.entityId,
                &talas::project::ScoreAutomationLane::laneId);
            if (found != score.automation.end())
                commandLane = *found;
        }
        else if (commandSelection.kind == TimelineScoreSelectionKind::Marker)
        {
            const auto found = std::ranges::find(
                score.markers, commandSelection.entityId,
                &talas::project::ScoreMarker::markerId);
            if (found != score.markers.end())
                commandMarker = *found;
        }
        if (timelineScoreHasCommand(
                viewport_.commands, TimelineScoreCommand::AddTrack))
            menu.addItem(30, "Add Track (N)");
        if (timelineScoreHasCommand(
                viewport_.commands, TimelineScoreCommand::AddAutomation)
            && canAddGainAutomation())
            menu.addItem(31, "Add Gain Automation (A)");
        // The Action menu's Record to disk...
        if (timelineRecorder_ && !bridge_.editor().document().timeline.score.clips.empty())
            menu.addItem(35, "Record to disk...");
        if (timelineScoreHasCommand(
                viewport_.commands, TimelineScoreCommand::DeleteSelection))
            menu.addItem(1, "Delete");
        // Draw automation freehand; minus removes a
        // clip's last effect.
        menu.addItem(61, "Pencil", true, pencilMode_);
        if (selection_.kind == TimelineScoreSelectionKind::Clip)
            menu.addItem(60, "Remove Last Effect (-)");
        if (selection_.kind == TimelineScoreSelectionKind::Track)
        {
            menu.addSeparator();
            if (timelineScoreHasCommand(
                    viewport_.commands, TimelineScoreCommand::ToggleMute))
                menu.addItem(2, "Toggle Mute");
            if (timelineScoreHasCommand(
                    viewport_.commands, TimelineScoreCommand::ToggleSolo))
                menu.addItem(3, "Toggle Solo");
            const auto& tracks = bridge_.editor().document().timeline.score.tracks;
            const auto selected = std::ranges::find(
                tracks, selection_.entityId, &talas::project::ScoreTrack::trackId);
            if (timelineScoreHasCommand(
                    viewport_.commands, TimelineScoreCommand::ChangeOutput)
                && selected != tracks.end()
                && selected->kind != talas::project::ScoreTrackKind::Master)
            {
                juce::PopupMenu routes;
                for (const auto& track : tracks)
                {
                    if (track.trackId == selected->trackId
                        || track.kind == talas::project::ScoreTrackKind::Track
                        || track.channelCount != selected->channelCount)
                    {
                        continue;
                    }
                    auto cursor = &track;
                    auto cyclic = false;
                    for (std::size_t depth = 0;
                         cursor != nullptr && depth <= tracks.size();
                         ++depth)
                    {
                        if (cursor->trackId == selected->trackId)
                        {
                            cyclic = true;
                            break;
                        }
                        if (cursor->outputTrackId.empty())
                            break;
                        const auto next = std::ranges::find(
                            tracks, cursor->outputTrackId,
                            &talas::project::ScoreTrack::trackId);
                        cursor = next == tracks.end() ? nullptr : &*next;
                    }
                    if (cyclic)
                        continue;
                    routeIds.push_back(track.trackId);
                    routes.addItem(
                        100 + static_cast<int>(routeIds.size()),
                        juceString(track.displayName + " · " + track.trackId),
                        selected->outputTrackId != track.trackId,
                        selected->outputTrackId == track.trackId);
                }
                if (!routeIds.empty())
                    menu.addSubMenu("Route Output", routes);
            }
            // A Sound track's input, no input or one
            // of the submixes it may take.
            if (selected != tracks.end() && selected->kind == talas::project::ScoreTrackKind::Track)
            {
                juce::PopupMenu inputs;
                const auto noInput = selected->inputTrackId.empty() && selected->inputChannel == 0;
                inputs.addItem(inputMenuBase, "No input", !noInput, noInput);
                for (const auto& choice : talas::app::timelineTrackInputChoices(bridge_.editor().document().timeline.score, selected->trackId))
                {
                    inputIds.push_back(choice.trackId);
                    inputs.addColouredItem(inputMenuBase + static_cast<int>(inputIds.size()), juceString(choice.displayName),
                        submixColour(choice.trackId), selected->inputTrackId != choice.trackId,
                        selected->inputTrackId == choice.trackId);
                }
                // The hardware's audio inputs.
                inputs.addSeparator();
                for (std::uint32_t channel = 1; channel <= talas::project::maximumScoreInputChannels; ++channel)
                    inputs.addItem(hardwareInputMenuBase + static_cast<int>(channel), "Input: " + juce::String(static_cast<int>(channel)),
                        selected->inputChannel != channel, selected->inputChannel == channel);
                menu.addSubMenu("Input", inputs);
                juce::PopupMenu submixes;
                for (const auto& item : talas::app::timelineSubmixMenu(bridge_.editor().document().timeline.score, selected->trackId))
                {
                    submixToggles.emplace_back(item.trackId, !item.connected);
                    submixes.addColouredItem(submixMenuBase + static_cast<int>(submixToggles.size()), juceString(item.displayName),
                        submixColourAt(item.colourIndex), !item.output, item.connected);
                }
                submixes.addSeparator();
                submixes.addItem(submixMenuBase, "New Submix...");
                menu.addSubMenu("Submix", submixes);
                // The audio output menu, the pan controls or
                // fixed output channels.
                juce::PopupMenu outputs;
                const auto lit = talas::app::timelineOutputStatus(bridge_.editor().document().timeline.score, selected->trackId);
                outputs.addItem(outputMenuBase, "Use Pan Controls", true, lit.empty());
                // No output, for a track heard through its sends.
                outputs.addItem(outputMenuBase + 50, "No Output", true, selected->noOutput);
                const auto& masterTrack = *std::ranges::find(tracks, talas::project::ScoreTrackKind::Master, &talas::project::ScoreTrack::kind);
                for (std::uint32_t channel = 1; channel <= masterTrack.channelCount; ++channel)
                    outputs.addItem(outputMenuBase + static_cast<int>(channel), "Output " + juce::String(static_cast<int>(channel)),
                        true, !lit.empty() && lit[channel - 1]);
                menu.addSubMenu("Output", outputs);
                // The MIDI status control's menu.
                juce::PopupMenu channels;
                channels.addItem(midiMenuBase, "Default Channel", true, selected->midiChannel == 0);
                for (int channel = 1; channel <= 16; ++channel)
                    channels.addItem(midiMenuBase + channel, "Channel " + juce::String(channel), true,
                        selected->midiChannel == channel);
                menu.addSubMenu("MIDI Channel", channels);
            }
            if (timelineScoreHasCommand(
                    viewport_.commands, TimelineScoreCommand::AddClip)
                && contextPointerFrame_.has_value())
            {
                juce::PopupMenu sounds;
                for (const auto& definition :
                     bridge_.editor().document().soundDefinitions)
                {
                    clipDefinitionIds.push_back(definition.definitionId);
                    sounds.addItem(
                        200 + static_cast<int>(clipDefinitionIds.size()),
                        juceString(definition.displayName + " · "
                            + definition.definitionId));
                }
                if (!clipDefinitionIds.empty())
                    menu.addSubMenu("Add Clip from Sound", sounds);
            }
        }
        else if (selection_.kind == TimelineScoreSelectionKind::Clip)
        {
            menu.addSeparator();
            const auto& clip = *commandClip;
            if (clipArtifactState_ && clipArtifactAction_)
            {
                const auto artifactState = clipArtifactState_(clip.clipId);
                using State = TimelineScoreClipArtifactStateKind;
                if (artifactState.kind == State::RenderingCache
                    || artifactState.kind == State::Freezing
                    || artifactState.kind == State::Cancelling)
                {
                    menu.addItem(54, "Cancel Clip Artifact Operation");
                }
                else if (artifactState.kind == State::Frozen)
                {
                    menu.addItem(53, "Unfreeze Clip");
                }
                else
                {
                    menu.addItem(
                        artifactState.kind == State::Live ? 50 : 51,
                        artifactState.kind == State::Live
                            ? "Cache Clip"
                            : "Refresh Cache",
                        artifactState.sourceAvailable,
                        false);
                    menu.addItem(
                        52,
                        "Freeze Clip",
                        artifactState.sourceAvailable,
                        false);
                }
                menu.addSectionHeader(
                    juceString(artifactState.detail.empty()
                        ? artifactState.label
                        : artifactState.detail));
                menu.addSeparator();
            }
            const auto repeated = multiplyScoreTime(
                clip.duration, clip.repeatCount);
            const auto fadeTotal = addScoreTime(clip.fadeIn, clip.fadeOut);
            const auto fadeCapacity = repeated.has_value()
                && fadeTotal.has_value() && *fadeTotal != *repeated;
            const auto fadeCanIncrease = [this, &repeated](
                                             const auto& fade,
                                             const auto& other) {
                if (!repeated.has_value())
                    return false;
                const auto adjusted = addScoreTime(
                    fade, nudgeAmount(fade.domain, false));
                const auto remaining = subtractScoreTime(*repeated, other);
                return adjusted.has_value() && remaining.has_value()
                    && minimumScoreTime(*adjusted, *remaining) != fade;
            };
            const auto fadeCanDecrease = [this](const auto& fade) {
                return subtractScoreTime(
                    fade, nudgeAmount(fade.domain, false)).has_value();
            };
            auto increasedRepeat = clip;
            const auto canIncreaseRepeat = clip.repeatCount
                    < talas::project::maximumScoreClipRepeats
                && (++increasedRepeat.repeatCount,
                    clipProjectsAtActiveRate(increasedRepeat));
            menu.addItem(10, "Increase Repeat", canIncreaseRepeat);
            menu.addItem(11, "Decrease Repeat", clip.repeatCount > 1U);
            menu.addItem(12, "Increase Fade In", fadeCapacity
                && fadeCanIncrease(clip.fadeIn, clip.fadeOut));
            menu.addItem(13, "Decrease Fade In", fadeCanDecrease(clip.fadeIn));
            menu.addItem(14, "Increase Fade Out", fadeCapacity
                && fadeCanIncrease(clip.fadeOut, clip.fadeIn));
            menu.addItem(15, "Decrease Fade Out", fadeCanDecrease(clip.fadeOut));
            // The VCS's values as the clip's controls.
            menu.addItem(16, "Set Live Controls to Current Values");
            // The Sound's polyphony.
            const auto voices = talas::app::timelineClipPolyphony(bridge_.editor().document(), clip.clipId);
            menu.addItem(62, voices.has_value() ? "Polyphony (" + juce::String(static_cast<int>(*voices)) + " voices)..."
                                                : juce::String{"Polyphony..."});
        }
        else if (selection_.kind == TimelineScoreSelectionKind::AutomationPoint)
        {
            menu.addSeparator();
            menu.addItem(20, "Toggle Step / Linear");
            juce::PopupMenu curves;
            const auto* lane = selectedAutomationLane();
            const auto hasNext = lane != nullptr
                && selection_.pointIndex + 1 < lane->points.size();
            curves.addItem(curveExponential, "Exponential", hasNext);
            curves.addItem(curveLogarithmic, "Logarithmic", hasNext);
            curves.addItem(curveSCurve, "S-Curve", hasNext);
            menu.addSubMenu("Curve to Next Point", curves, hasNext);
        }
        auto automationTargets = selectedAutomationTargets();
        const auto automationOwner = selectedAutomationOwner();
        if (automationOwner.has_value() && !automationTargets.empty()
            && timelineScoreHasCommand(
                viewport_.commands, TimelineScoreCommand::AddAutomation))
        {
            juce::PopupMenu automate;
            for (std::size_t index = 0; index < automationTargets.size(); ++index)
            {
                automate.addItem(
                    automateBase + static_cast<int>(index),
                    automationTargetLabel(automationTargets[index]));
            }
            // Notes, the track's note events, always live.
            for (const auto live : talas::app::automationLiveControls(automationOwner->scope))
            {
                automate.addSeparator();
                automate.addItem(-1, juceString(std::string{live}) + " (always live from MIDI, not recorded)", false);
            }
            menu.addSubMenu(
                automationOwner->scope == talas::project::ScoreAutomationScope::Clip
                    ? "Automate in Clip"
                    : "Automate",
                automate);
        }
        if (const auto* lane = selectedAutomationLane();
            lane != nullptr && !lane->points.empty())
        {
            juce::PopupMenu transforms;
            // Each pressed shows ticked; pressed again it is
            // taken away.
            using Transform = talas::app::AutomationTransform;
            const auto pressed = [&](Transform transform) { return talas::app::automationLaneTransformed(*lane, transform); };
            transforms.addItem(transformInvert, "Invert...", true, pressed(Transform::Invert));
            transforms.addItem(transformReverse, "Reverse", true, pressed(Transform::Reverse));
            transforms.addItem(transformLoop, "Loop...", true, pressed(Transform::Loop));
            transforms.addItem(transformSquareOff, "Square Off", true, pressed(Transform::SquareOff));
            transforms.addItem(transformScale, "Scale...", true, pressed(Transform::Scale));
            transforms.addItem(transformOffset, "Offset...", true, pressed(Transform::Offset));
            transforms.addItem(transformThreshold, "Threshold...", true, pressed(Transform::Threshold));
            transforms.addItem(transformTimeScale, "Time Scale...", true, pressed(Transform::TimeScale));
            transforms.addItem(transformSpray, "Spray...", true, pressed(Transform::Spray));
            transforms.addItem(transformQuantize, "Quantize Endpoints...", true, pressed(Transform::QuantizeEndpoints));
            transforms.addItem(transformScaleToFit, "Scale to Fit",
                lane->scope == talas::project::ScoreAutomationScope::Clip, pressed(Transform::ScaleToFit));
            // A copy of the transformed or slaved function as the
            // lane's own points.
            transforms.addSeparator();
            transforms.addItem(transformKeep, "Keep Transformed Function",
                !lane->transforms.empty() || !lane->masterLaneId.empty());
            menu.addSubMenu("Transform Automation", transforms);
        }
        if (const auto* lane = selectedAutomationLane())
        {
            juce::PopupMenu states;
            states.addItem(laneStateBase + 1, "Live", true, lane->state == talas::project::ScoreAutomationState::Live);
            states.addItem(laneStateBase, "Automated", true, lane->state == talas::project::ScoreAutomationState::Automated);
            states.addItem(laneStateBase + 2, "Record Enable", true, lane->state == talas::project::ScoreAutomationState::Record);
            // Promote to master.
            states.addSeparator();
            states.addItem(takeMenuBase - 4, "Promote to Master", lane->scope != talas::project::ScoreAutomationScope::Master
                && lane->masterLaneId.empty());
            menu.addSubMenu("Lane State", states);
            // The Source of Control, the lane's
            // takes, New Take and Rename take.
            juce::PopupMenu sources;
            for (const auto& name : talas::app::automationTakeNames(*lane))
            {
                takeNames.push_back(name);
                sources.addItem(takeMenuBase + static_cast<int>(takeNames.size()), juceString(name), true, name == lane->takeName);
            }
            sources.addSeparator();
            sources.addItem(takeMenuBase - 2, "New Take");
            sources.addItem(takeMenuBase - 1, "Rename Take...");
            // A live lane's MIDI channel and controller.
            if (lane->state != talas::project::ScoreAutomationState::Automated)
                sources.addItem(takeMenuBase - 8,
                    "MIDI " + juceString(talas::app::timelineLaneControlSource(bridge_.editor().document(), *lane)) + "...");
            menu.addSubMenu("Source of Control", sources);
            // The State menu's Slave to, the potential masters.
            juce::PopupMenu masters;
            masters.addItem(masterMenuBase, "Not Slaved", true, lane->masterLaneId.empty());
            for (const auto& masterId : talas::project::scoreAutomationMasterCandidates(score, lane->laneId))
            {
                const auto master = std::ranges::find(score.automation, masterId, &talas::project::ScoreAutomationLane::laneId);
                masterIds.push_back(masterId);
                masters.addItem(masterMenuBase + static_cast<int>(masterIds.size()),
                    juceString(master->eventName + (master->scope == talas::project::ScoreAutomationScope::Master ? " · master"
                                                    : master->scope == talas::project::ScoreAutomationScope::Track ? " · track"
                                                                                                                : "")),
                    true, lane->masterLaneId == masterId);
            }
            menu.addSubMenu("Slave to", masters);
            // The lane's vertical zoom.
            menu.addItem(takeMenuBase - 6, "Zoom Values Out");
            menu.addItem(takeMenuBase - 7, "Zoom Values In");
        }
        {
            juce::PopupMenu automation;
            for (const auto& [offset, label] : {std::pair{1, "Live Control"}, std::pair{0, "Automated"}, std::pair{2, "Record Enable"}})
            {
                juce::PopupMenu groups;
                groups.addItem(groupStateBase + offset * 2, "Parameter Controls");
                groups.addItem(groupStateBase + offset * 2 + 1, "Track Level & Angle Controls");
                automation.addSubMenu(label, groups);
            }
            // One tempo for the whole Timeline.
            automation.addSeparator();
            automation.addItem(groupStateBase + 6, "Set Master BPM...");
            menu.addSubMenu("Automation", automation);
        }
        // The selected Sound aligned to begin or end
        // at another's anchor.
        std::vector<std::string> alignTargets;
        if (commandClip.has_value())
        {
            juce::PopupMenu starts;
            juce::PopupMenu ends;
            const auto& document = bridge_.editor().document();
            for (const auto& other : score.clips)
            {
                if (other.clipId == commandClip->clipId || alignTargets.size() >= maximumAlignTargets)
                    continue;
                const auto info = talas::app::timelineSoundInfo(document, other.clipId);
                const auto label = juceString((info.has_value() ? info->name : other.clipId) + " · "
                    + talas::app::timelineTimeText(talas::app::clipAnchorTime(other), document.timeline.musicalTime));
                const auto id = alignBase + 2 * static_cast<int>(alignTargets.size());
                starts.addItem(id, label);
                ends.addItem(id + 1, label);
                alignTargets.push_back(other.clipId);
            }
            menu.addSubMenu("Align Start to Anchor of", starts, !alignTargets.empty());
            menu.addSubMenu("Align End to Anchor of", ends, !alignTargets.empty());
        }
        // The transport's jumps from the cursor.
        juce::PopupMenu jumps;
        jumps.addItem(30, "Previous Marker");
        jumps.addItem(31, "Next Marker");
        jumps.addItem(32, "Previous Important Time");
        jumps.addItem(33, "Next Important Time");
        menu.addSubMenu("Jump", jumps);
        const auto recordingActions = controllerRecordingMenuState();
        menu.addSeparator();
        menu.addItem(
            40,
            armedControllerRecordingTake_.has_value()
                ? "Stop Controller Recording (E)"
                : "Start Controller Recording (E)",
            recordingActions.startOrStopEnabled);
        menu.addItem(
            41,
            "Preview Recording Simplification (V)",
            recordingActions.previewEnabled);
        menu.addItem(
            42,
            "Approve Exact Preview (Return)",
            recordingActions.approveEnabled);
        menu.addItem(
            43,
            "Discard Raw Recording",
            recordingActions.discardEnabled);
        if (menu.getNumItems() == 0)
            return;
        juce::Component::SafePointer<ScoreViewportComponent> safeThis(this);
        menu.showMenuAsync(
            juce::PopupMenu::Options{}.withTargetComponent(this),
            [safeThis,
             routeIds = std::move(routeIds),
             inputIds = std::move(inputIds),
             takeNames = std::move(takeNames),
             masterIds = std::move(masterIds),
             submixToggles = std::move(submixToggles),
             clipDefinitionIds = std::move(clipDefinitionIds),
             contextPointerFrame = contextPointerFrame_,
             commandSelection,
             commandFocusIdentity,
             commandTrack = std::move(commandTrack),
             commandClip = std::move(commandClip),
             commandLane = std::move(commandLane),
             commandMarker = std::move(commandMarker),
             automationTargets = std::move(automationTargets),
             alignTargets = std::move(alignTargets),
             automationOwner](int result) {
                if (safeThis == nullptr || result == 0)
                    return;
                const auto& current = safeThis->bridge_.editor()
                                          .document().timeline.score;
                const auto exactEntityStillExists = [&]() {
                    if (commandTrack.has_value())
                    {
                        const auto found = std::ranges::find(
                            current.tracks, commandTrack->trackId,
                            &talas::project::ScoreTrack::trackId);
                        return found != current.tracks.end()
                            && *found == *commandTrack;
                    }
                    if (commandClip.has_value())
                    {
                        const auto found = std::ranges::find(
                            current.clips, commandClip->clipId,
                            &talas::project::ScoreClip::clipId);
                        return found != current.clips.end()
                            && *found == *commandClip;
                    }
                    if (commandLane.has_value())
                    {
                        const auto found = std::ranges::find(
                            current.automation, commandLane->laneId,
                            &talas::project::ScoreAutomationLane::laneId);
                        return found != current.automation.end()
                            && *found == *commandLane;
                    }
                    if (commandMarker.has_value())
                    {
                        const auto found = std::ranges::find(
                            current.markers, commandMarker->markerId,
                            &talas::project::ScoreMarker::markerId);
                        return found != current.markers.end()
                            && *found == *commandMarker;
                    }
                    return commandSelection.kind == TimelineScoreSelectionKind::None;
                }();
                if (!exactEntityStillExists)
                {
                    safeThis->onStatus_(
                        "Score context action cancelled because its target changed");
                    return;
                }
                safeThis->selection_ = commandSelection;
                safeThis->selectedFocusIdentity_ = commandFocusIdentity;
                if (result == 1)
                    (void) safeThis->deleteSelection();
                else if (result == 30)
                    (void) safeThis->addScoreTrack();
                else if (result == 31)
                    (void) safeThis->addScoreAutomationLane();
                else if (result == 35)
                    safeThis->askToRecordTimeline();
                else if (result == 60)
                    (void) safeThis->removeSelectedClipEffect();
                else if (result == 61)
                    safeThis->setPencilMode(!safeThis->pencilMode_);
                else if (result == 2)
                    (void) safeThis->toggleSelectedTrackMute();
                else if (result == 3)
                    (void) safeThis->toggleSelectedTrackSolo();
                else if (result == 10)
                    (void) safeThis->adjustSelectedClipRepeat(1);
                else if (result == 11)
                    (void) safeThis->adjustSelectedClipRepeat(-1);
                else if (result == 12)
                    (void) safeThis->adjustSelectedClipFade(true, 1);
                else if (result == 13)
                    (void) safeThis->adjustSelectedClipFade(true, -1);
                else if (result == 14)
                    (void) safeThis->adjustSelectedClipFade(false, 1);
                else if (result == 15)
                    (void) safeThis->adjustSelectedClipFade(false, -1);
                else if (result == 20)
                    (void) safeThis->toggleSelectedAutomationInterpolation();
                else if (result >= automateBase
                    && static_cast<std::size_t>(result - automateBase) < automationTargets.size()
                    && automationOwner.has_value())
                {
                    (void) safeThis->addAutomationLaneFor(
                        automationOwner->scope,
                        automationOwner->ownerId,
                        automationTargets[static_cast<std::size_t>(result - automateBase)]);
                }
                else if (result >= curveExponential && result <= curveSCurve)
                {
                    (void) safeThis->shapeSelectedAutomationSegment(
                        result == curveExponential
                            ? talas::app::AutomationCurveShape::Exponential
                            : result == curveLogarithmic
                            ? talas::app::AutomationCurveShape::Logarithmic
                            : talas::app::AutomationCurveShape::SCurve);
                }
                else if (result == transformReverse)
                    (void) safeThis->transformSelectedAutomationLane(
                        talas::app::AutomationTransform::Reverse, {});
                else if (result == transformSquareOff)
                    (void) safeThis->transformSelectedAutomationLane(
                        talas::app::AutomationTransform::SquareOff, {});
                else if (result == transformScaleToFit)
                    (void) safeThis->scaleSelectedAutomationLaneToItsClip();
                else if (result == transformKeep)
                    (void) safeThis->keepSelectedAutomationFunction();
                else if (result >= transformInvert && result <= transformQuantize)
                    safeThis->promptAutomationTransform(result);
                else if (result == 16)
                    (void) safeThis->setSelectedClipLiveControlsToCurrentValues();
                else if (result == 62)
                    safeThis->askForSelectedClipPolyphony();
                else if (result >= 30 && result <= 33)
                    (void) safeThis->jumpCursor(result - 30);
                else if (result >= laneStateBase && result <= groupStateBase + 5)
                    (void) safeThis->performLaneStateMenuAction(result);
                else if (result >= alignBase
                    && static_cast<std::size_t>(result - alignBase) / 2 < alignTargets.size())
                    (void) safeThis->alignSelectedClip(alignTargets[static_cast<std::size_t>(result - alignBase) / 2],
                        (result - alignBase) % 2 == 0 ? talas::app::ClipAlignment::StartToAnchor
                                                      : talas::app::ClipAlignment::EndToAnchor);
                else if (result >= 50 && result <= 54
                    && safeThis->clipArtifactAction_)
                {
                    auto action = TimelineScoreClipArtifactAction::Cache;
                    if (result == 51)
                        action = TimelineScoreClipArtifactAction::RefreshCache;
                    else if (result == 52)
                        action = TimelineScoreClipArtifactAction::Freeze;
                    else if (result == 53)
                        action = TimelineScoreClipArtifactAction::Unfreeze;
                    else if (result == 54)
                        action = TimelineScoreClipArtifactAction::Cancel;
                    safeThis->clipArtifactAction_(
                        commandSelection.entityId, action);
                }
                else if (result >= 40 && result <= 43)
                    (void) safeThis->performControllerRecordingMenuAction(result);
                else if (result == takeMenuBase - 3)
                    safeThis->askForNewMasterControl();
                else if (result == takeMenuBase - 5)
                    (void) safeThis->makeSoundOfTimeline();
                else if ((result == takeMenuBase - 6 || result == takeMenuBase - 7) && commandLane.has_value())
                    (void) safeThis->zoomLaneValues(commandLane->laneId, result == takeMenuBase - 6 ? 1 : -1);
                else if (result == takeMenuBase - 4)
                    (void) safeThis->promoteSelectedLaneToMaster();
                else if (result >= masterMenuBase && static_cast<std::size_t>(result - masterMenuBase) <= masterIds.size())
                    (void) safeThis->slaveSelectedLane(
                        result == masterMenuBase ? std::string{} : masterIds[static_cast<std::size_t>(result - masterMenuBase - 1)]);
                else if (result == takeMenuBase - 2)
                    (void) safeThis->newSelectedLaneTake();
                else if (result == takeMenuBase - 1)
                    safeThis->askToRenameSelectedLaneTake();
                else if (result == takeMenuBase - 8)
                    safeThis->askSelectedLaneControlSource();
                else if (result > takeMenuBase && static_cast<std::size_t>(result - takeMenuBase) <= takeNames.size())
                    (void) safeThis->chooseSelectedLaneTake(takeNames[static_cast<std::size_t>(result - takeMenuBase - 1)]);
                else if (result > hardwareInputMenuBase
                    && result <= hardwareInputMenuBase + static_cast<int>(talas::project::maximumScoreInputChannels))
                    (void) safeThis->setTrackHardwareInput(commandSelection.entityId,
                        static_cast<std::uint32_t>(result - hardwareInputMenuBase));
                else if (result >= midiMenuBase && result <= midiMenuBase + 16)
                    (void) safeThis->setTrackMidiChannel(commandSelection.entityId, static_cast<std::uint8_t>(result - midiMenuBase));
                else if (result == outputMenuBase + 50)
                    (void) safeThis->setTrackNoOutput(commandSelection.entityId);
                else if (result >= outputMenuBase && result <= outputMenuBase + 8)
                    (void) safeThis->toggleTrackOutput(commandSelection.entityId, static_cast<std::uint32_t>(result - outputMenuBase));
                else if (result == submixMenuBase)
                    safeThis->askForNewSubmix(commandSelection.entityId);
                else if (result > submixMenuBase
                    && static_cast<std::size_t>(result - submixMenuBase) <= submixToggles.size())
                {
                    const auto& [submixId, connect] = submixToggles[static_cast<std::size_t>(result - submixMenuBase - 1)];
                    (void) safeThis->setTrackSend(commandSelection.entityId, submixId, connect);
                }
                else if (result >= inputMenuBase
                    && static_cast<std::size_t>(result - inputMenuBase) <= inputIds.size())
                {
                    (void) safeThis->setTrackInput(commandSelection.entityId,
                        result == inputMenuBase ? std::string{} : inputIds[static_cast<std::size_t>(result - inputMenuBase - 1)]);
                }
                else if (result >= 101
                    && static_cast<std::size_t>(result - 101) < routeIds.size())
                {
                    (void) safeThis->routeSelectedTrack(
                        routeIds[static_cast<std::size_t>(result - 101)]);
                }
                else if (result >= 201
                    && static_cast<std::size_t>(result - 201)
                        < clipDefinitionIds.size()
                    && contextPointerFrame.has_value())
                {
                    (void) safeThis->createClipAtFrame(
                        commandSelection.entityId,
                        *contextPointerFrame,
                        false,
                        clipDefinitionIds[
                            static_cast<std::size_t>(result - 201)]);
                }
            });
    }

    void updateScrollRanges()
    {
        if (!projection_)
            return;
        const auto sampleRate = std::max<std::uint32_t>(1, runtime_.sampleRate);
        const auto minimumExtent = static_cast<std::uint64_t>(sampleRate) * 8U;
        const auto playheadExtent = runtime_.playheadFrame
            > std::numeric_limits<std::uint64_t>::max()
                - static_cast<std::uint64_t>(sampleRate) * 2U
            ? std::numeric_limits<std::uint64_t>::max()
            : runtime_.playheadFrame
                + static_cast<std::uint64_t>(sampleRate) * 2U;
        const auto contentExtent = std::max({
            minimumExtent,
            projection_->contentEndFrame(),
            playheadExtent,
        });
        scrollContentEndFrame_ = contentExtent;
        const auto visibleFrames = std::max<double>(
            1.0,
            frameToScrollValue(contentExtent) / zoom_.getValue());
        horizontal_.setRangeLimits(
            0.0,
            frameToScrollValue(contentExtent));
        horizontal_.setCurrentRange(
            horizontal_.getCurrentRangeStart(),
            visibleFrames,
            juce::dontSendNotification);
        const auto body = contentBounds().withTrimmedTop(scoreRulerHeight());
        const auto visibleRows = std::max(
            1,
            body.getHeight() / scoreRowHeight());
        vertical_.setRangeLimits(
            0.0,
            static_cast<double>(std::max<std::size_t>(
                projection_->rowCount(),
                static_cast<std::size_t>(visibleRows))));
        vertical_.setCurrentRange(
            vertical_.getCurrentRangeStart(),
            static_cast<double>(visibleRows),
            juce::dontSendNotification);
    }

    void rebuildViewport()
    {
        if (!projection_)
        {
            viewport_ = {};
            return;
        }
        if (!projection_->containsSelection(selection_))
        {
            selection_ = {};
            selectedFocusIdentity_.clear();
        }
        const auto body = contentBounds().withTrimmedTop(scoreRulerHeight());
        const auto width = std::max(1, body.getWidth() - scoreHeaderWidth());
        firstVisibleRow_ = static_cast<std::size_t>(std::max(
            0.0,
            std::floor(vertical_.getCurrentRangeStart())));
        auto firstFrame = scrollValueToFrame(
            horizontal_.getCurrentRangeStart());
        const auto visibleFrames = std::max<std::uint64_t>(
            1,
            scrollValueToFrame(std::ceil(
                horizontal_.getCurrentRangeSize())));
        const auto rangeEnd = horizontal_.getCurrentRangeStart()
            + horizontal_.getCurrentRangeSize();
        const auto atExactEnd = rangeEnd
            >= horizontal_.getMaximumRangeLimit();
        auto endFrame = firstFrame
                > std::numeric_limits<std::uint64_t>::max() - visibleFrames
            ? std::numeric_limits<std::uint64_t>::max()
            : firstFrame + visibleFrames;
        if (atExactEnd && scrollContentEndFrame_ > endFrame)
        {
            endFrame = scrollContentEndFrame_;
            firstFrame = horizontal_.getCurrentRangeStart() <= 0.0
                ? 0
                : endFrame > visibleFrames
                ? endFrame - visibleFrames
                : 0;
        }
        viewportStartFrame_ = firstFrame;
        viewportEndFrame_ = endFrame;
        const auto rowCount = std::min<std::size_t>(
            maximumTimelineViewportRows,
            static_cast<std::size_t>(
                std::max(1, body.getHeight() / scoreRowHeight() + 1)));
        viewport_ = projection_->query({
            firstFrame,
            endFrame,
            firstVisibleRow_,
            rowCount,
            static_cast<double>(width),
            static_cast<double>(scoreRowHeight()),
            rulerMode(),
        }, selection_);
        if (selection_.kind != TimelineScoreSelectionKind::None)
        {
            const auto selectedItem = std::ranges::find(
                viewport_.items, true, &TimelineScoreItemProjection::selected);
            if (selectedItem != viewport_.items.end())
                selectedFocusIdentity_ = selectedItem->focusIdentity;
        }
        rebuildAccessibilityEntities();
        updateAccessibleTitle();
        refreshInfoStrip();
    }

    [[nodiscard]] std::string rowFocusIdentity(
        const TimelineScoreRowProjection& row) const
    {
        return row.kind == TimelineScoreRowKind::Automation
            ? "timeline.automation:" + row.stableId
            : "timeline.track:" + row.stableId;
    }

    void rebuildAccessibilityEntities()
    {
        auto body = contentBounds().withTrimmedTop(scoreRulerHeight());
        accessibilityContainer_.setBounds(body);
        const auto* focusedBefore = juce::Component::getCurrentlyFocusedComponent();
        const auto restoreFocusedSelection = focusedBefore != nullptr
            && (focusedBefore == &accessibilityContainer_
                || accessibilityContainer_.isParentOf(focusedBefore));
        struct Desired
        {
            TimelineScoreSelection selection;
            std::string focusIdentity;
            juce::String title;
            juce::Rectangle<int> bounds;
        };
        std::vector<Desired> desired;
        desired.reserve(viewport_.rows.size() + viewport_.items.size());
        for (std::size_t index = 0; index < viewport_.rows.size(); ++index)
        {
            const auto& row = viewport_.rows[index];
            desired.push_back({
                {row.kind == TimelineScoreRowKind::Automation
                        ? TimelineScoreSelectionKind::AutomationLane
                        : TimelineScoreSelectionKind::Track,
                    row.stableId,
                    0},
                rowFocusIdentity(row),
                juceString(row.accessibilityText),
                {0,
                 static_cast<int>(index) * scoreRowHeight(),
                 scoreHeaderWidth(),
                 scoreRowHeight()},
            });
        }
        for (const auto& item : viewport_.items)
        {
            if (item.kind == TimelineScoreItemKind::AutomationSegment)
                continue;
            TimelineScoreSelection selection;
            selection.entityId = item.stableId;
            selection.pointIndex = item.sourceIndex;
            switch (item.kind)
            {
            case TimelineScoreItemKind::Clip:
                selection.kind = TimelineScoreSelectionKind::Clip;
                break;
            case TimelineScoreItemKind::AutomationPoint:
                selection.kind = TimelineScoreSelectionKind::AutomationPoint;
                break;
            case TimelineScoreItemKind::Marker:
                selection.kind = TimelineScoreSelectionKind::Marker;
                break;
            case TimelineScoreItemKind::AutomationSegment:
                continue;
            }
            const auto localRow = item.row >= firstVisibleRow_
                ? item.row - firstVisibleRow_
                : 0;
            desired.push_back({
                std::move(selection),
                item.focusIdentity,
                juceString(item.accessibilityText)
                    + (item.kind == TimelineScoreItemKind::Clip
                            && clipArtifactState_
                        ? ". Artifact state "
                            + juceString(
                                clipArtifactState_(item.stableId).detail)
                        : juce::String{}),
                {scoreHeaderWidth()
                        + static_cast<int>(std::floor(item.x)),
                 static_cast<int>(localRow) * scoreRowHeight(),
                 std::max(8, static_cast<int>(std::ceil(item.width))),
                 item.kind == TimelineScoreItemKind::Marker
                    ? std::max(scoreRowHeight(), static_cast<int>(std::ceil(item.height)))
                    : scoreRowHeight()},
            });
        }
        std::vector<std::string> nextOrder;
        nextOrder.reserve(desired.size());
        for (const auto& entry : desired)
            nextOrder.push_back(entry.focusIdentity);
        const auto selectedIdentityVisible = selectedFocusIdentity_.empty()
            || std::ranges::find(nextOrder, selectedFocusIdentity_)
                != nextOrder.end();
        const auto structureChanged = nextOrder != accessibilityIdentityOrder_;
        if (structureChanged)
        {
            accessibilityEntities_.clear(true);
            accessibilityIdentityOrder_ = nextOrder;
            for (const auto& entry : desired)
            {
                auto* entity = accessibilityEntities_.add(
                    new AccessibilityEntity(
                        *this,
                        entry.selection,
                        entry.focusIdentity,
                        entry.title));
                accessibilityContainer_.addAndMakeVisible(entity);
            }
        }
        for (std::size_t index = 0; index < desired.size(); ++index)
        {
            accessibilityEntities_[static_cast<int>(index)]->update(
                desired[index].selection,
                desired[index].title,
                desired[index].bounds);
        }
        if (structureChanged)
        {
            if (auto* handler = getAccessibilityHandler())
            {
                handler->notifyAccessibilityEvent(
                    juce::AccessibilityEvent::structureChanged);
            }
            if (restoreFocusedSelection)
            {
                if (selectedFocusIdentity_.empty() || !selectedIdentityVisible)
                    grabKeyboardFocus();
                else
                    focusAccessibleEntity(selectedFocusIdentity_);
            }
        }
    }

    void selectAccessibleEntity(
        const TimelineScoreSelection& selection,
        std::string_view focusIdentity)
    {
        selection_ = selection;
        selectedFocusIdentity_ = std::string(focusIdentity);
        captureSelectedAutomationPoint();
        rebuildViewport();
        onStatus_("Selected Score " + juceString(selection_.entityId));
        repaint();
    }

    void focusAccessibleEntity(std::string_view focusIdentity)
    {
        if (focusIdentity.empty())
            return;
        for (auto* entity : accessibilityEntities_)
        {
            if (entity->focusIdentity() == focusIdentity)
            {
                if (entity->isShowing())
                    entity->grabKeyboardFocus();
                return;
            }
        }
    }

    bool handleAccessibleKey(
        const juce::KeyPress& key,
        std::string_view currentIdentity)
    {
        const auto code = key.getKeyCode();
        if (key.getModifiers().isAltDown())
            return keyPressed(key);
        if (code != juce::KeyPress::leftKey
            && code != juce::KeyPress::rightKey
            && code != juce::KeyPress::upKey
            && code != juce::KeyPress::downKey)
            return keyPressed(key);
        if (accessibilityEntities_.isEmpty())
            return false;
        auto index = 0;
        for (; index < accessibilityEntities_.size(); ++index)
        {
            if (accessibilityEntities_[index]->focusIdentity() == currentIdentity)
                break;
        }
        const auto forward = code == juce::KeyPress::rightKey
            || code == juce::KeyPress::downKey;
        if (index >= accessibilityEntities_.size())
            index = forward ? 0 : accessibilityEntities_.size() - 1;
        else
            index = std::clamp(
                index + (forward ? 1 : -1),
                0,
                accessibilityEntities_.size() - 1);
        accessibilityEntities_[index]->grabKeyboardFocus();
        return true;
    }

    void updateAccessibleTitle()
    {
        auto title = juceString(viewport_.accessibilityText);
        if (selection_.kind != TimelineScoreSelectionKind::None)
        {
            auto selectionText = juceString(selection_.entityId);
            const auto item = std::ranges::find_if(
                viewport_.items,
                [](const auto& candidate) {
                    return candidate.selected;
                });
            const auto row = std::ranges::find_if(
                viewport_.rows,
                [](const auto& candidate) {
                    return candidate.selected;
                });
            if (item != viewport_.items.end())
                selectionText = juceString(item->accessibilityText);
            else if (row != viewport_.rows.end())
                selectionText = juceString(row->accessibilityText);
            title += " Selected " + selectionText + ".";
        }
        setTitle(title);
        setDescription(title);
    }

    void scrollBarMoved(juce::ScrollBar*, double) override
    {
        rebuildViewport();
        repaint();
    }

    void captureSelectedAutomationPoint()
    {
        selectedPointPosition_.reset();
        if (selection_.kind != TimelineScoreSelectionKind::AutomationPoint)
            return;
        const auto& lanes = bridge_.editor().document().timeline.score.automation;
        const auto lane = std::ranges::find(
            lanes, selection_.entityId,
            &talas::project::ScoreAutomationLane::laneId);
        if (lane != lanes.end() && selection_.pointIndex < lane->points.size())
            selectedPointPosition_ = lane->points[selection_.pointIndex].position;
    }

    [[nodiscard]] const talas::project::ScoreAutomationLane* selectedPointLane() const
    {
        if (selection_.kind != TimelineScoreSelectionKind::AutomationPoint)
            return nullptr;
        const auto& lanes = bridge_.editor().document().timeline.score.automation;
        const auto lane = std::ranges::find(lanes, selection_.entityId, &talas::project::ScoreAutomationLane::laneId);
        return lane == lanes.end() ? nullptr : &*lane;
    }

    // The selected point and those selected with it, in increasing order.
    [[nodiscard]] std::vector<std::size_t> selectedPointIndices() const
    {
        const auto* lane = selectedPointLane();
        if (lane == nullptr || selection_.pointIndex >= lane->points.size())
            return {};
        std::vector<std::size_t> indices{selection_.pointIndex};
        if (extraPointsLaneId_ == selection_.entityId)
        {
            for (const auto index : extraPoints_)
            {
                if (index < lane->points.size())
                    indices.push_back(index);
            }
        }
        std::ranges::sort(indices);
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
        return indices;
    }

    [[nodiscard]] bool pointSelectedWithOthers(std::string_view laneId, std::size_t index) const
    {
        return extraPointsLaneId_ == laneId && selection_.kind == TimelineScoreSelectionKind::AutomationPoint
            && selection_.entityId == laneId && std::ranges::find(extraPoints_, index) != extraPoints_.end();
    }

    void selectPoints(const std::string& laneId, std::vector<std::size_t> indices)
    {
        if (indices.empty())
            return;
        std::ranges::sort(indices);
        selection_ = {TimelineScoreSelectionKind::AutomationPoint, laneId, indices.front()};
        extraPointsLaneId_ = laneId;
        extraPoints_.assign(indices.begin() + 1, indices.end());
        captureSelectedAutomationPoint();
    }

    void clearExtraPoints()
    {
        extraPointsLaneId_.clear();
        extraPoints_.clear();
        extraClips_.clear();
    }

    // The selected Sound and those selected with it.
    [[nodiscard]] std::vector<std::string> selectedClipIds() const
    {
        if (selection_.kind != TimelineScoreSelectionKind::Clip)
            return {};
        const auto& clips = bridge_.editor().document().timeline.score.clips;
        std::vector<std::string> ids{selection_.entityId};
        for (const auto& id : extraClips_)
        {
            if (std::ranges::find(ids, id) == ids.end() && std::ranges::find(clips, id, &talas::project::ScoreClip::clipId) != clips.end())
                ids.push_back(id);
        }
        return ids;
    }

    void selectClips(std::vector<std::string> ids)
    {
        if (ids.empty())
            return;
        selection_ = {TimelineScoreSelectionKind::Clip, ids.front(), 0};
        extraPointsLaneId_.clear();
        extraPoints_.clear();
        extraClips_.assign(ids.begin() + 1, ids.end());
    }

    void toggleSelectedClip(const std::string& clipId)
    {
        auto ids = selectedClipIds();
        const auto found = std::ranges::find(ids, clipId);
        if (found == ids.end())
            ids.push_back(clipId);
        else if (ids.size() > 1)
            ids.erase(found);
        selectClips(std::move(ids));
    }

    // The group's edits as one, or within a held nudge as part of it.
    bool commitClipGroup(std::vector<talas::project::ScoreClip> clips, juce::String status)
    {
        talas::project::ProjectScoreEditBatch batch;
        batch.clips = std::move(clips);
        if (gestureToken_.has_value())
        {
            const auto result = bridge_.editor().applyScoreEdits(std::move(batch), *gestureToken_);
            if (!result.ok())
            {
                onStatus_(juceString(result.message));
                return true;
            }
            gestureChanged_ = true;
            rebuild(bridge_.editor().document(), runtime_, true);
            onStatus_(std::move(status));
            return true;
        }
        return commitDiscreteEdit(bridge_.editor().applyScoreEdits(std::move(batch)), std::move(status));
    }

    bool deleteSelectedClips()
    {
        const auto ids = selectedClipIds();
        if (ids.size() < 2)
            return false;
        talas::project::ProjectScoreEditBatch batch;
        batch.removeClipIds = ids;
        const auto result = bridge_.editor().applyScoreEdits(std::move(batch));
        if (result.ok())
        {
            selection_ = {};
            selectedFocusIdentity_.clear();
            clearExtraPoints();
        }
        return commitDiscreteEdit(result, "Deleted the selected Sounds");
    }

    // A box drawn over the Sounds: those it touches.
    bool selectClipsInBox(juce::Rectangle<float> box)
    {
        const auto rows = contentBounds().withTrimmedTop(scoreRulerHeight());
        const auto rowHeight = scoreRowHeight();
        std::vector<std::string> ids;
        for (const auto& item : viewport_.items)
        {
            if (item.kind != TimelineScoreItemKind::Clip || item.row < firstVisibleRow_)
                continue;
            const auto bounds = juce::Rectangle<float>{
                static_cast<float>(rows.getX() + scoreHeaderWidth() + item.x),
                static_cast<float>(rows.getY() + static_cast<int>(item.row - firstVisibleRow_) * rowHeight + 4),
                static_cast<float>(std::max(2.0, item.width)), static_cast<float>(rowHeight - 8)};
            if (box.intersects(bounds) && std::ranges::find(ids, item.stableId) == ids.end())
                ids.push_back(item.stableId);
        }
        if (ids.empty())
            return false;
        selectClips(std::move(ids));
        selectedFocusIdentity_.clear();
        rebuildViewport();
        onStatus_("Selected " + juce::String(static_cast<int>(selectedClipIds().size())) + " Sounds");
        return true;
    }

    [[nodiscard]] bool clipSelectedWithOthers(std::string_view clipId) const
    {
        return selection_.kind == TimelineScoreSelectionKind::Clip && std::ranges::find(extraClips_, clipId) != extraClips_.end();
    }

    // Shift+click adds a point to the selection or takes it out.
    void toggleSelectedPoint(std::size_t index)
    {
        auto indices = selectedPointIndices();
        const auto found = std::ranges::find(indices, index);
        if (found == indices.end())
            indices.push_back(index);
        else if (indices.size() > 1)
            indices.erase(found);
        selectPoints(selection_.entityId, std::move(indices));
    }

    // The selected points as one edit, staying selected where
    // they land.
    bool commitSelectedPoints(std::optional<talas::project::ScoreAutomationLane> edited,
        const std::vector<double>& landedTimes, juce::String status)
    {
        if (!edited.has_value())
        {
            onStatus_("The selected points cannot take that edit");
            return true;
        }
        const auto laneId = edited->laneId;
        if (gestureToken_.has_value())
        {
            // Within a held nudge, as one owned gesture committed at its end.
            const auto result = bridge_.editor().updateScoreAutomationLane(*edited, *gestureToken_);
            if (!result.ok())
            {
                onStatus_(juceString(result.message));
                return true;
            }
            selectPoints(laneId, talas::app::automationPointsAt(*edited, landedTimes));
            gestureChanged_ = true;
            rebuild(bridge_.editor().document(), runtime_, true);
            onStatus_(std::move(status));
            return true;
        }
        const auto result = bridge_.editor().updateScoreAutomationLane(*edited);
        if (result.ok())
            selectPoints(laneId, talas::app::automationPointsAt(*edited, landedTimes));
        return commitDiscreteEdit(result, std::move(status));
    }

    // Enter, then a value for all the selected points.
    void askSelectedPointsValue()
    {
        const auto* lane = selectedPointLane();
        if (lane == nullptr)
            return;
        auto* window = new juce::AlertWindow("Points' Value", "Type the value for the selected points.",
            juce::MessageBoxIconType::NoIcon, this);
        window->addTextEditor("value", juce::String(lane->points[selection_.pointIndex].value));
        window->addButton("OK", 1, juce::KeyPress{juce::KeyPress::returnKey});
        window->addButton("Cancel", 0, juce::KeyPress{juce::KeyPress::escapeKey});
        juce::Component::SafePointer<ScoreViewportComponent> safeThis(this);
        window->enterModalState(true,
            juce::ModalCallbackFunction::create([safeThis, window](int result) {
                if (safeThis != nullptr && result == 1)
                    (void) safeThis->setSelectedPointsValue(window->getTextEditorContents("value").trim().toStdString());
            }),
            true);
    }

public:
    bool setSelectedPointsValue(const std::string& text)
    {
        const auto* lane = selectedPointLane();
        double value = 0.0;
        const auto parsed = talas::fromChars(text.data(), text.data() + text.size(), value);
        if (lane == nullptr || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        {
            onStatus_("Type a number for the selected points");
            return false;
        }
        const auto indices = selectedPointIndices();
        std::vector<double> times;
        for (const auto index : indices)
            times.push_back(talas::app::automationPointTime(lane->points[index].position));
        return commitSelectedPoints(talas::app::setAutomationPointValues(*lane, indices, value), times,
            "Set the selected points to " + juce::String(value));
    }

    // The selected points moved together, the first to the time.
    bool moveSelectedPointsTo(double firstTime)
    {
        const auto* lane = selectedPointLane();
        const auto indices = selectedPointIndices();
        if (lane == nullptr || indices.empty())
            return false;
        const auto offset = firstTime - talas::app::automationPointTime(lane->points[indices.front()].position);
        std::vector<double> times;
        for (const auto index : indices)
            times.push_back(talas::app::automationPointTime(lane->points[index].position) + offset);
        return commitSelectedPoints(talas::app::moveAutomationPoints(*lane, indices, firstTime), times,
            "Moved the selected points");
    }

    bool deleteSelectedPoints()
    {
        const auto* lane = selectedPointLane();
        const auto indices = selectedPointIndices();
        if (lane == nullptr || indices.size() < 2)
            return false;
        auto edited = talas::app::deleteAutomationPoints(*lane, indices);
        if (!edited.has_value())
        {
            onStatus_("A lane keeps at least one point");
            return true;
        }
        const auto laneId = lane->laneId;
        const auto result = bridge_.editor().updateScoreAutomationLane(*edited);
        if (result.ok())
        {
            selection_ = {TimelineScoreSelectionKind::AutomationLane, laneId, 0};
            selectedFocusIdentity_ = "timeline.automation:" + laneId;
            selectedPointPosition_.reset();
            clearExtraPoints();
        }
        return commitDiscreteEdit(result, "Deleted the selected points");
    }

    // A box drawn over a lane, from one corner to the other in this
    // component's coordinates: the points in it.
    bool selectPointsInBox(std::size_t row, juce::Rectangle<float> box)
    {
        const auto rows = contentBounds().withTrimmedTop(scoreRulerHeight());
        const auto rowHeight = scoreRowHeight();
        std::string laneId;
        std::vector<std::size_t> indices;
        for (const auto& item : viewport_.items)
        {
            if (item.kind != TimelineScoreItemKind::AutomationPoint || item.row != row || item.row < firstVisibleRow_)
                continue;
            const auto top = static_cast<float>(rows.getY() + static_cast<int>(item.row - firstVisibleRow_) * rowHeight + 4);
            const auto centre = juce::Point<float>{
                static_cast<float>(rows.getX() + scoreHeaderWidth() + item.x + std::max(2.0, item.width) / 2.0),
                top + static_cast<float>(rowHeight - 8) * (1.0F - static_cast<float>(item.displayValue))};
            if (box.contains(centre))
            {
                laneId = item.stableId;
                indices.push_back(item.sourceIndex);
            }
        }
        if (indices.empty())
            return false;
        selectPoints(laneId, std::move(indices));
        selectedFocusIdentity_.clear();
        rebuildViewport();
        onStatus_("Selected " + juce::String(static_cast<int>(selectedPointIndices().size())) + " points");
        return true;
    }

    // All of a lane's points.
    bool selectAllPointsOfLane()
    {
        const auto laneId = selection_.entityId;
        if (selection_.kind != TimelineScoreSelectionKind::AutomationLane
            && selection_.kind != TimelineScoreSelectionKind::AutomationPoint)
            return false;
        const auto& lanes = bridge_.editor().document().timeline.score.automation;
        const auto lane = std::ranges::find(lanes, laneId, &talas::project::ScoreAutomationLane::laneId);
        if (lane == lanes.end() || lane->points.empty())
            return false;
        std::vector<std::size_t> indices(lane->points.size());
        std::iota(indices.begin(), indices.end(), std::size_t{0});
        selectPoints(laneId, std::move(indices));
        rebuildViewport();
        repaint();
        return true;
    }

    [[nodiscard]] std::vector<std::size_t> selectedPointsForSmoke() const { return selectedPointIndices(); }

    bool selectPointsForSmoke(std::string_view laneId, std::vector<std::size_t> indices)
    {
        const auto& lanes = bridge_.editor().document().timeline.score.automation;
        const auto lane = std::ranges::find(lanes, laneId, &talas::project::ScoreAutomationLane::laneId);
        if (lane == lanes.end() || indices.empty()
            || std::ranges::any_of(indices, [&](std::size_t index) { return index >= lane->points.size(); }))
            return false;
        selectPoints(std::string{laneId}, std::move(indices));
        rebuildViewport();
        return true;
    }

    bool boxLaneForSmoke(std::string_view laneId)
    {
        const auto row = std::ranges::find(viewport_.rows, laneId, &TimelineScoreRowProjection::stableId);
        if (row == viewport_.rows.end())
            return false;
        const auto rowOffset = static_cast<int>(row - viewport_.rows.begin());
        const auto rows = contentBounds().withTrimmedTop(scoreRulerHeight());
        const auto box = juce::Rectangle<int>{rows.getX() + scoreHeaderWidth(), rows.getY() + rowOffset * scoreRowHeight(),
            rows.getWidth() - scoreHeaderWidth(), scoreRowHeight()};
        return selectPointsInBox(firstVisibleRow_ + static_cast<std::size_t>(rowOffset), box.toFloat());
    }

    [[nodiscard]] std::vector<std::string> selectedClipsForSmoke() const { return selectedClipIds(); }

    bool shiftClickClipForSmoke(const std::string& clipId)
    {
        if (selection_.kind != TimelineScoreSelectionKind::Clip)
            return false;
        toggleSelectedClip(clipId);
        rebuildViewport();
        return true;
    }

    void selectForSmoke(TimelineScoreSelection selection)
    {
        selection_ = std::move(selection);
        clearExtraPoints();
        rebuildViewport();
    }

    [[nodiscard]] std::uint32_t sampleRateForSmoke() const noexcept { return activeSampleRate(); }

    bool addPointForSmoke(std::string_view laneId, std::uint64_t frame, double height)
    {
        selection_ = {TimelineScoreSelectionKind::AutomationLane, std::string{laneId}, 0};
        clearExtraPoints();
        return addAutomationPointAtFrame(frame, false, height);
    }

private:

    void reconcileSelectedAutomationPoint(
        const talas::project::ProjectDocument& document)
    {
        if (selection_.kind != TimelineScoreSelectionKind::AutomationPoint)
        {
            selectedPointPosition_.reset();
            return;
        }
        if (!selectedPointPosition_.has_value())
        {
            selection_ = {};
            selectedFocusIdentity_.clear();
            return;
        }
        const auto lane = std::ranges::find(
            document.timeline.score.automation,
            selection_.entityId,
            &talas::project::ScoreAutomationLane::laneId);
        if (lane == document.timeline.score.automation.end())
        {
            selection_ = {};
            selectedFocusIdentity_.clear();
            selectedPointPosition_.reset();
            return;
        }
        const auto point = std::ranges::find(
            lane->points,
            *selectedPointPosition_,
            &talas::project::ScoreAutomationPoint::position);
        if (point == lane->points.end())
        {
            selection_ = {};
            selectedFocusIdentity_.clear();
            selectedPointPosition_.reset();
            return;
        }
        selection_.pointIndex = static_cast<std::size_t>(
            std::distance(lane->points.begin(), point));
    }

    enum class GestureMode
    {
        None,
        Move,
        ResizeEnd,
        TrimStart,
    };

    talas::runtime::ProjectRuntimeBridge& bridge_;
    DocumentChangeCallback onDocumentChange_;
    StatusCallback onStatus_;
    AcceptedStageCurrentCallback acceptedStageCurrent_;
    ClipArtifactStateCallback clipArtifactState_;
    ClipArtifactActionCallback clipArtifactAction_;
    juce::ComboBox rulerMode_;
    juce::ComboBox framesMenu_;
    // The Grid and Quantization menus.
    juce::ComboBox gridMode_;
    juce::ComboBox quantization_;
    talas::app::TimelineGridMode gridModeValue_ = talas::app::TimelineGridMode::Free;
    talas::app::TimelineQuantization quantizationValue_{};
    juce::ComboBox programChannel_;
    talas::app::TimelineMarks marks_;
    juce::Slider zoom_;
    juce::TextButton zoomOut_{"-"};
    juce::TextButton zoomIn_{"+"};
    juce::TextButton zoomFit_{"F"};
    juce::TextButton timeSource_{"Free running"};
    // The Cache menu, the doughnut.
    juce::TextButton cacheMenu_{"Cache"};
    juce::TextButton audioMenu_{"Audio"};
    static constexpr int cacheFormatItemBase = 100;
    static constexpr int sharingAcrossTracksItem = 200;
    TimelinePanel::TimeSourceChooser timeSourceChooser_;
    TimelinePanel::DragImporter dragImporter_;
    TimelinePanel::SpaceKey spaceKey_;
    TimelineInfoStrip infoStrip_;
    juce::ScrollBar horizontal_{false};
    juce::ScrollBar vertical_{true};
    TimelineScoreRuntimeState runtime_;
    std::unique_ptr<TimelineScoreProjection> projection_;
    TimelineScoreViewportProjection viewport_;
    TimelineScoreSelection selection_;
    std::string selectedFocusIdentity_;
    std::optional<talas::project::ScoreTime> selectedPointPosition_;
    // The points Shift+clicked or boxed with
    // the selected point, in its lane, and the box being drawn.
    TimelinePanel::EnvelopeExtractor envelopeExtractor_;
    TimelinePanel::TimelineRecorder timelineRecorder_;
    double recordingProgress_ = 0.0;
    std::unique_ptr<juce::AlertWindow> recordingThermometer_;
    std::unique_ptr<juce::TextButton> recordingStopButton_;
    std::shared_ptr<const talas::runtime::PreparedScoreSchedule> panningSchedule_;
    std::unique_ptr<juce::FileChooser> recordingChooser_;
    std::shared_ptr<std::atomic<bool>> recordingCancelled_ = std::make_shared<std::atomic<bool>>(false);
    std::unique_ptr<juce::ThreadPool> recordingPool_;
    // The Sounds Shift+clicked or boxed with the
    // selected Sound, and whether the box being drawn is for Sounds.
    std::vector<std::string> extraClips_;
    bool boxForClips_ = false;
    std::string extraPointsLaneId_;
    std::vector<std::size_t> extraPoints_;
    std::optional<juce::Point<float>> boxStart_;
    std::optional<juce::Point<float>> boxEnd_;
    std::size_t boxRow_ = 0;
    std::optional<std::uint64_t> lastSnapPreviewFrame_;
    std::optional<std::uint64_t> contextPointerFrame_;
    // The pencil and the stroke being drawn.
    bool pencilMode_ = false;
    std::string strokeLaneId_;
    float strokeRowTop_ = 0.0F;
    std::vector<std::pair<std::uint64_t, double>> stroke_;
    std::size_t firstVisibleRow_ = 0;
    std::uint64_t scrollContentEndFrame_ = 0;
    AccessibilityContainer accessibilityContainer_;
    juce::OwnedArray<AccessibilityEntity> accessibilityEntities_;
    std::vector<std::string> accessibilityIdentityOrder_;
    std::optional<talas::project::ProjectEditor::HistoryTransactionToken>
        gestureToken_;
    GestureMode gestureMode_ = GestureMode::None;
    std::optional<talas::project::ScoreClip> gestureClip_;
    std::optional<talas::project::ScoreMarker> gestureMarker_;
    std::optional<talas::project::ScoreAutomationLane> gestureLane_;
    std::uint64_t gesturePointerFrame_ = 0;
    std::uint64_t gestureEntityFrame_ = 0;
    std::vector<std::pair<std::string, int>> laneValueZooms_;
    talas::app::TimelineClipsCopy clipsCopied_;
    // The project the clips were copied in, so
    // that they paste into another Timeline with the Sounds they play.
    std::shared_ptr<const talas::project::ProjectDocument> clipsCopiedFrom_;
    std::optional<std::filesystem::path> clipsCopiedFromFile_;
    const talas::core::NodeRegistry* pasteRegistry_ = nullptr;
    std::function<std::optional<std::filesystem::path>()> projectFile_;
public:
    // A Sound copied elsewhere, made one of the project's Sounds to paste.
    std::function<std::optional<std::string>()> soundToPaste_;
    std::function<void(std::vector<std::string>)> soundsCopied_;
    std::function<void(const std::string&)> soundToEdit_;
    // Something copied elsewhere since: Paste takes it, not these clips.
    void forgetCopiedClips()
    {
        clipsCopied_ = {};
        clipsCopiedFrom_.reset();
        clipsCopiedFromFile_.reset();
    }
    void setPasteSources(const talas::core::NodeRegistry& registry,
        std::function<std::optional<std::filesystem::path>()> projectFile)
    {
        pasteRegistry_ = &registry;
        projectFile_ = std::move(projectFile);
    }
    // The clips copied, and the project they were copied in.
    void keepCopiedClips(talas::app::TimelineClipsCopy copied)
    {
        clipsCopied_ = std::move(copied);
        clipsCopiedFrom_ = std::make_shared<const talas::project::ProjectDocument>(bridge_.editor().document());
        clipsCopiedFromFile_ = projectFile_ ? projectFile_() : std::nullopt;
    }
private:
    std::string pasteTrackId_;
    std::optional<std::uint64_t> pasteFrame_;
    juce::Point<float> gestureStartPosition_;
    std::optional<talas::app::AutomationDragAxis> gestureAxis_;
    std::uint64_t viewportStartFrame_ = 0;
    std::uint64_t viewportEndFrame_ = 1;
    bool gestureChanged_ = false;
    bool keyboardGesture_ = false;
    std::optional<std::uint64_t> armedControllerRecordingTake_;
    std::optional<talas::project::ControllerRecordingTake>
        controllerRecordingTake_;
    std::optional<talas::project::ControllerRecordingSimplificationPreview>
        controllerRecordingPreview_;
    std::uint64_t nextControllerRecordingPreviewGeneration_ = 1;
    bool discardControllerRecordingOnCollection_ = false;
};

// The Timeline's lower half, its controls
// editor: the line along its top dragged up or down shares the height with
// the score; the Timeline's controls are listed on the left and the
// selected one's function is drawn to their right.
class ControlsEditorComponent final : public juce::Component, private juce::ListBoxModel
{
public:
    static constexpr int lineHeight = 6;
    static constexpr int transformButtonHeight = 20;
    std::function<void(int)> onLineDragged;
    std::function<void(talas::project::ScoreAutomationLane)> onLaneEdited;
    // A transformation button pressed for
    // the selected control.
    std::function<void(const std::string&, talas::app::AutomationTransform)> onTransformPressed;

    ControlsEditorComponent()
    {
        list_.setModel(this);
        list_.setRowHeight(18);
        list_.setColour(juce::ListBox::backgroundColourId, panelColour);
        list_.setColour(juce::ListBox::outlineColourId, panelBorderColour);
        list_.setOutlineThickness(1);
        describeControl(list_, "Timeline controls", "The Timeline's controls; select one to see its function beside the list.");
        addAndMakeVisible(list_);
        // The transformation buttons arrayed across the bottom, each
        // with fly-by help saying what it would do.
        for (const auto& button : talas::app::automationTransformButtons())
        {
            auto made = std::make_unique<juce::TextButton>(juceString(std::string{button.label}));
            made->setClickingTogglesState(false);
            made->onClick = [this, transform = button.transform] {
                if (const auto* lane = selectedLane(); lane != nullptr && onTransformPressed)
                    onTransformPressed(lane->laneId, transform);
            };
            addAndMakeVisible(*made);
            transformButtons_.emplace_back(button.transform, std::move(made));
        }
        updateTransformButtons();
    }

    void setScore(const talas::project::ProjectScore& score)
    {
        score_ = score;
        list_.updateContent();
        if (score_.automation.empty())
            selected_ = -1;
        else if (selected_ < 0 || selected_ >= static_cast<int>(score_.automation.size()))
            selected_ = 0;
        list_.selectRow(selected_, false, true);
        updateTransformButtons();
        repaint();
    }

    [[nodiscard]] std::optional<std::string> selectedLaneForSmoke() const
    {
        if (selected_ < 0 || selected_ >= static_cast<int>(score_.automation.size()))
            return std::nullopt;
        return score_.automation[static_cast<std::size_t>(selected_)].laneId;
    }

    void resized() override
    {
        auto bounds = getLocalBounds();
        bounds.removeFromTop(lineHeight);
        list_.setBounds(bounds.removeFromLeft(std::min(180, bounds.getWidth() / 3)));
        // The transformation buttons across the bottom of the editor.
        auto buttons = bounds.removeFromBottom(transformButtonHeight + 4).reduced(6, 2);
        const auto count = static_cast<int>(transformButtons_.size());
        for (int index = 0; index < count; ++index)
        {
            const auto left = buttons.getX() + buttons.getWidth() * index / std::max(1, count);
            const auto right = buttons.getX() + buttons.getWidth() * (index + 1) / std::max(1, count);
            transformButtons_[static_cast<std::size_t>(index)].second->setBounds(
                left, buttons.getY(), std::max(0, right - left - 2), buttons.getHeight());
        }
        graph_ = bounds.reduced(6, 4);
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(panelColour);
        // The dragged dividing line, a short grip in its middle.
        const auto divider = getLocalBounds().removeFromTop(lineHeight).reduced(0, 2).toFloat();
        graphics.setColour(juce::Colours::white.withAlpha(0.06F));
        graphics.fillRect(divider);
        graphics.setColour(juce::Colours::white.withAlpha(0.22F));
        graphics.fillRoundedRectangle(divider.withSizeKeepingCentre(36.0F, std::min(3.0F, divider.getHeight())), 1.5F);
        if (!graph_.isEmpty())
        {
            WorkbenchStyle::paintWell(graphics, graph_.toFloat().expanded(2.0F), 5.0F);
            WorkbenchStyle::paintDisplayGrid(graphics, graph_.toFloat(), 8, 4);
        }
        if (selected_ < 0 || selected_ >= static_cast<int>(score_.automation.size()) || graph_.isEmpty())
            return;
        const auto& lane = score_.automation[static_cast<std::size_t>(selected_)];
        const auto curve = talas::app::timelineControlsCurve(score_, lane, 0.0, shownEnd(lane), static_cast<std::size_t>(graph_.getWidth()));
        juce::Path path;
        for (std::size_t column = 0; column < curve.size(); ++column)
        {
            const auto x = static_cast<float>(graph_.getX()) + static_cast<float>(column);
            const auto y = static_cast<float>(graph_.getBottom()) - static_cast<float>(curve[column].height) * static_cast<float>(graph_.getHeight());
            column == 0 ? path.startNewSubPath(x, y) : path.lineTo(x, y);
        }
        if (!path.isEmpty())
        {
            auto under = path;
            under.lineTo(static_cast<float>(graph_.getX() + static_cast<int>(curve.size()) - 1),
                static_cast<float>(graph_.getBottom()));
            under.lineTo(static_cast<float>(graph_.getX()), static_cast<float>(graph_.getBottom()));
            under.closeSubPath();
            graphics.setGradientFill(juce::ColourGradient{controlColour.withAlpha(0.24F), 0.0F,
                static_cast<float>(graph_.getY()), controlColour.withAlpha(0.0F), 0.0F,
                static_cast<float>(graph_.getBottom()), false});
            graphics.fillPath(under);
        }
        WorkbenchStyle::strokeWithGlow(graphics, path, controlColour, 1.5F, 6.0F);
        graphics.setColour(secondaryTextColour);
        graphics.setFont(WorkbenchType::micro());
        graphics.drawText(juceString(lane.eventName), graph_.reduced(6, 4).withHeight(14), juce::Justification::topLeft);
    }

    void mouseMove(const juce::MouseEvent& event) override
    {
        setMouseCursor(event.y < lineHeight ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::NormalCursor);
    }

    void mouseDown(const juce::MouseEvent& event) override
    {
        draggingLine_ = event.y < lineHeight;
        // The lower half creates and edits the controls: a Ctrl+Click
        // in the graph adds a point at the time and value clicked.
        if (draggingLine_ || !(event.mods.isCommandDown() || event.mods.isCtrlDown()) || !graph_.contains(event.getPosition())
            || selected_ < 0 || selected_ >= static_cast<int>(score_.automation.size()) || !onLaneEdited)
            return;
        const auto& lane = score_.automation[static_cast<std::size_t>(selected_)];
        if (auto added = talas::app::timelineControlsPointAdded(lane, 0.0, shownEnd(lane),
                static_cast<double>(event.x - graph_.getX()) / std::max(1, graph_.getWidth()),
                static_cast<double>(graph_.getBottom() - event.y) / std::max(1, graph_.getHeight())))
            onLaneEdited(std::move(*added));
    }

    void mouseDrag(const juce::MouseEvent& event) override
    {
        if (draggingLine_ && onLineDragged && getParentComponent() != nullptr)
            onLineDragged(event.getEventRelativeTo(getParentComponent()).y);
    }

private:
    // The graph shows the lane from the start to its last point, at least
    // one second or tick.
    [[nodiscard]] double shownEnd(const talas::project::ScoreAutomationLane& lane) const
    {
        auto end = 1.0;
        for (const auto& point : talas::app::automationShownPoints(score_, lane))
            end = std::max(end, talas::app::automationPointTime(point.position));
        return end;
    }

    int getNumRows() override { return static_cast<int>(score_.automation.size()); }

    void paintListBoxItem(int row, juce::Graphics& graphics, int width, int height, bool rowSelected) override
    {
        if (row < 0 || row >= static_cast<int>(score_.automation.size()))
            return;
        if (rowSelected)
            graphics.fillAll(selectionColour.withAlpha(0.35F));
        const auto& lane = score_.automation[static_cast<std::size_t>(row)];
        graphics.setColour(textColour);
        graphics.setFont(WorkbenchType::micro());
        graphics.drawText(juceString(lane.eventName + "  " + lane.ownerId), 4, 0, width - 8, height,
            juce::Justification::centredLeft);
    }

    void selectedRowsChanged(int row) override
    {
        selected_ = row;
        updateTransformButtons();
        repaint();
    }

    [[nodiscard]] const talas::project::ScoreAutomationLane* selectedLane() const
    {
        if (selected_ < 0 || selected_ >= static_cast<int>(score_.automation.size()))
            return nullptr;
        return &score_.automation[static_cast<std::size_t>(selected_)];
    }

    // Each button's fly-by help for the selected control, and pressed
    // while it transforms that control.
    void updateTransformButtons()
    {
        const auto* lane = selectedLane();
        for (auto& [transform, button] : transformButtons_)
        {
            button->setTooltip(juceString(talas::app::automationTransformButtonHelp(lane, transform)));
            button->setToggleState(lane != nullptr && talas::app::automationLaneTransformed(*lane, transform),
                juce::dontSendNotification);
        }
    }

    juce::ListBox list_;
    talas::project::ProjectScore score_;
    juce::Rectangle<int> graph_;
    int selected_ = -1;
    bool draggingLine_ = false;
    std::vector<std::pair<talas::app::AutomationTransform, std::unique_ptr<juce::TextButton>>> transformButtons_;
};

class TimelinePanelImpl final : public TimelinePanel,
                                private juce::ListBoxModel,
                                private juce::ScrollBar::Listener
{
public:
    using DocumentChangeCallback = std::function<void()>;
    using StatusCallback = std::function<void(const juce::String&)>;

    enum class ContentMode
    {
        Events,
        MusicalMap,
        Score,
    };

    struct MusicalRow
    {
        bool tempo = true;
        std::size_t sourceIndex = 0;
        std::uint64_t tick = 0;
    };

    TimelinePanelImpl(talas::runtime::ProjectRuntimeBridge& bridge,
                  DocumentChangeCallback onDocumentChange,
                  StatusCallback onStatus,
                  TimelinePanel::OutputMeterSnapshotCallback outputMeters,
                  TimelinePanel::AcceptedStageCurrentCallback acceptedStageCurrent,
                  TimelinePanel::ClipArtifactStateCallback clipArtifactState,
                  TimelinePanel::ClipArtifactActionCallback clipArtifactAction)
        : bridge_(bridge)
        , onDocumentChange_(std::move(onDocumentChange))
        , onStatus_(std::move(onStatus))
        , outputMeters_(std::move(outputMeters))
        , scoreViewport_(
              bridge_,
              onDocumentChange_,
              onStatus_,
              std::move(acceptedStageCurrent),
              std::move(clipArtifactState),
              std::move(clipArtifactAction))
        , loopApplyButton_("Apply Loop")
        , addButton_("Add")
        , updateButton_("Update")
        , removeButton_("Delete Event")
    {
        setFocusContainerType(juce::Component::FocusContainerType::none);
        describeControl(
            *this,
            "Timeline",
            "Inspect the virtualized Score, or edit legacy Event Value and musical-map data. Score selection, zoom, scrolling, and rulers are keyboard accessible.");
        list_.setModel(this);
        list_.setRowHeight(28);
        list_.setColour(juce::ListBox::backgroundColourId, panelColour);
        list_.setColour(juce::ListBox::outlineColourId, panelBorderColour);
        list_.setOutlineThickness(1);
        describeControl(
            list_,
            "Timeline Event Values",
            "Event Value changes ordered by time. Select a row to edit it.");
        addAndMakeVisible(list_);
        addAndMakeVisible(scoreViewport_);
        addChildComponent(controlsEditor_);
        controlsEditor_.onLineDragged = [this](int y) {
            const auto top = scoreViewport_.getY();
            const auto height = controlsEditor_.getBottom() - top;
            controlsShare_ = talas::app::timelineControlsShare(y - top, height);
            resized();
        };
        controlsEditor_.onTransformPressed = [this](const std::string& laneId, talas::app::AutomationTransform transform) {
            (void) scoreViewport_.pressTransformationButton(laneId, transform);
            refresh();
        };
        controlsEditor_.onLaneEdited = [this](talas::project::ScoreAutomationLane lane) {
            const auto result = bridge_.editor().updateScoreAutomationLane(lane);
            onStatus_(result.ok() ? juce::String{"Added a point to the control"} : juceString(result.message));
            if (result.ok() && onDocumentChange_)
                onDocumentChange_();
            refresh();
        };
        addAndMakeVisible(
            laneScrollBar_);
        laneScrollBar_.addListener(this);
        laneScrollBar_.setAutoHide(true);
        laneScrollBar_.setSingleStepSize(
            minimumTimelineLaneHeight);
        addAndMakeVisible(loopButton_);
        addAndMakeVisible(loopStartInput_);
        addAndMakeVisible(loopEndInput_);
        addAndMakeVisible(loopApplyButton_);
        addAndMakeVisible(loopStartLabel_);
        addAndMakeVisible(loopEndLabel_);
        addAndMakeVisible(timeInput_);
        addAndMakeVisible(nameInput_);
        addAndMakeVisible(valueInput_);
        addAndMakeVisible(domainInput_);
        addAndMakeVisible(interpolationInput_);
        addAndMakeVisible(timeLabel_);
        addAndMakeVisible(nameLabel_);
        addAndMakeVisible(valueLabel_);
        addAndMakeVisible(domainLabel_);
        addAndMakeVisible(interpolationLabel_);
        addAndMakeVisible(addButton_);
        addAndMakeVisible(updateButton_);
        addAndMakeVisible(removeButton_);
        addAndMakeVisible(viewMode_);
        addAndMakeVisible(musicalKindInput_);
        addAndMakeVisible(musicalTickInput_);
        addAndMakeVisible(musicalPrimaryInput_);
        addAndMakeVisible(musicalSecondaryInput_);
        addAndMakeVisible(musicalKindLabel_);
        addAndMakeVisible(musicalTickLabel_);
        addAndMakeVisible(musicalPrimaryLabel_);
        addAndMakeVisible(musicalSecondaryLabel_);

        viewMode_.addItem("Events", 1);
        viewMode_.addItem("Musical Map", 2);
        viewMode_.addItem("Score", 3);
        viewMode_.setSelectedId(3, juce::dontSendNotification);
        musicalKindInput_.addItem("Tempo", 1);
        musicalKindInput_.addItem("Meter", 2);
        musicalKindInput_.setSelectedId(1, juce::dontSendNotification);
        musicalTickInput_.setTextToShowWhenEmpty("0", secondaryTextColour);
        musicalPrimaryInput_.setTextToShowWhenEmpty("120", secondaryTextColour);
        musicalSecondaryInput_.setTextToShowWhenEmpty("4", secondaryTextColour);

        loopButton_.setButtonText("Loop");
        loopStartInput_.setTextToShowWhenEmpty("0.000", secondaryTextColour);
        loopEndInput_.setTextToShowWhenEmpty("4.000", secondaryTextColour);
        timeInput_.setTextToShowWhenEmpty("0.000", secondaryTextColour);
        nameInput_.setTextToShowWhenEmpty("Cutoff", secondaryTextColour);
        valueInput_.setTextToShowWhenEmpty("440.0", secondaryTextColour);
        const std::array labelText = {
            std::pair{&loopStartLabel_, "Loop start (s)"},
            std::pair{&loopEndLabel_, "Loop end (s)"},
            std::pair{&timeLabel_, "Time (s)"},
            std::pair{&nameLabel_, "Event Value"},
            std::pair{&valueLabel_, "Value"},
            std::pair{&domainLabel_, "Domain"},
            std::pair{&interpolationLabel_, "Interpolation"},
        };
        for (const auto& [label, text] : labelText)
        {
            label->setText(text, juce::dontSendNotification);
            label->setColour(juce::Label::textColourId, secondaryTextColour);
            label->setFont(WorkbenchType::micro());
            label->setJustificationType(juce::Justification::centredLeft);
        }
        for (auto* label : {
                 &musicalKindLabel_,
                 &musicalTickLabel_,
                 &musicalPrimaryLabel_,
                 &musicalSecondaryLabel_,
             })
        {
            label->setColour(juce::Label::textColourId, secondaryTextColour);
            label->setFont(WorkbenchType::micro());
            label->setJustificationType(juce::Justification::centredLeft);
        }
        musicalKindLabel_.setText("Type", juce::dontSendNotification);
        musicalTickLabel_.setText("Tick (960 PPQ)", juce::dontSendNotification);
        musicalPrimaryLabel_.setText("BPM", juce::dontSendNotification);
        musicalSecondaryLabel_.setText("Denominator", juce::dontSendNotification);
        for (auto* editor : {
                 &loopStartInput_,
                 &loopEndInput_,
                 &timeInput_,
                 &nameInput_,
                 &valueInput_,
                 &musicalTickInput_,
                 &musicalPrimaryInput_,
                 &musicalSecondaryInput_,
             })
        {
            editor->setFont(WorkbenchType::body());
        }
        describeControl(
            loopStartInput_,
            "Loop start",
            "Loop start time in seconds.");
        describeControl(
            loopEndInput_,
            "Loop end",
            "Loop end time in seconds.");
        describeControl(
            timeInput_,
            "Event time",
            "Event time in seconds.");
        describeControl(
            nameInput_,
            "Event Value name",
            "Name of the Event Value to automate.");
        describeControl(
            valueInput_,
            "Event value",
            "Numeric Event Value at this time.");
        describeControl(
            domainInput_,
            "Event value domain",
            "Unit domain of the Event Value.");
        describeControl(
            interpolationInput_,
            "Interpolation",
            "Interpolation after this Timeline event.");
        describeControl(
            addButton_,
            "Add Timeline Event",
            "Add a Timeline Event Value from the labeled fields.");
        describeControl(
            updateButton_,
            "Update Timeline Event",
            "Apply the labeled field values to the selected Timeline event.");
        describeControl(
            removeButton_,
            "Delete Timeline Event",
            "Delete the selected Timeline Event Value.");
        describeControl(
            viewMode_,
            "Timeline editor mode",
            "Switch between the Score editor shell, seconds-based Event Values, and the canonical tempo and meter map.");
        describeControl(
            musicalKindInput_,
            "Musical event type",
            "Choose Tempo or Meter when adding a musical-map event.");
        describeControl(
            musicalTickInput_,
            "Musical tick",
            "Exact non-negative tick at 960 ticks per quarter note.");
        describeControl(
            musicalPrimaryInput_,
            "Tempo or meter numerator",
            "BPM for Tempo, or numerator for Meter.");
        describeControl(
            musicalSecondaryInput_,
            "Meter denominator",
            "Power-of-two denominator from 1 through 64 for Meter.");
        domainInput_.addItem("scalar", 1);
        domainInput_.addItem("hertz", 2);
        domainInput_.addItem("seconds", 3);
        domainInput_.addItem("note number", 4);
        domainInput_.addItem("samples", 5);
        domainInput_.setSelectedId(1, juce::dontSendNotification);
        interpolationInput_.addItem("step", 1);
        interpolationInput_.addItem("linear", 2);
        interpolationInput_.setSelectedId(
            1,
            juce::dontSendNotification);
        loopButton_.onClick = [this] { applyLoop(); };
        loopApplyButton_.onClick = [this] { applyLoop(); };
        loopStartInput_.onReturnKey = [this] { applyLoop(); };
        loopEndInput_.onReturnKey = [this] { applyLoop(); };
        viewMode_.onChange = [this] {
            contentMode_ = viewMode_.getSelectedId() == 3
                ? ContentMode::Score
                : viewMode_.getSelectedId() == 2
                ? ContentMode::MusicalMap
                : ContentMode::Events;
            preferredRow_ = -1;
            selectedKey_.reset();
            updateModeVisibility();
            refresh();
        };
        musicalKindInput_.onChange = [this] {
            updateMusicalFormLabels();
        };
        addButton_.onClick = [this] {
            contentMode_ == ContentMode::Events
                ? addEvent()
                : addMusicalEvent();
        };
        updateButton_.onClick = [this] {
            contentMode_ == ContentMode::Events
                ? updateEvent()
                : updateMusicalEvent();
        };
        removeButton_.onClick = [this] {
            contentMode_ == ContentMode::Events
                ? removeEvent()
                : removeMusicalEvent();
        };
        timeInput_.onReturnKey = [this] { selectedRow() >= 0 ? updateEvent() : addEvent(); };
        nameInput_.onReturnKey = [this] { selectedRow() >= 0 ? updateEvent() : addEvent(); };
        valueInput_.onReturnKey = [this] { selectedRow() >= 0 ? updateEvent() : addEvent(); };
        updateModeVisibility();
        refresh();
    }

    ~TimelinePanelImpl() override
    {
        laneScrollBar_.removeListener(this);
        if (dragTransaction_.has_value())
            bridge_.editor().endOwnedHistoryTransaction(*dragTransaction_);
    }

    void refresh() override
    {
        const auto& timeline = bridge_.editor().document().timeline;
        scoreViewport_.rebuild(
            bridge_.editor().document(),
            scoreRuntimeState(
                latestTransportSnapshot_,
                externalTransportDetail_,
                bridge_.editor().document(),
                outputMeters_ ? outputMeters_()
                    : TimelineOutputMeterSnapshot{}));
        controlsEditor_.setScore(timeline.score);
        const auto& events = timeline.eventValues;
        lanes_ =
            talas::project::buildTimelineLanes(
                events);
        rebuildMusicalRows();
        updateLaneScrollBar();
        loopButton_.setToggleState(
            timeline.loop.enabled,
            juce::dontSendNotification);
        loopStartInput_.setText(
            formatEditableDouble(timeline.loop.startSeconds),
            juce::dontSendNotification);
        loopEndInput_.setText(
            formatEditableDouble(timeline.loop.endSeconds),
            juce::dontSendNotification);
        auto targetRow = -1;
        if (contentMode_ == ContentMode::MusicalMap)
        {
            if (preferredRow_ >= 0 && !musicalRows_.empty())
            {
                targetRow = std::min(
                    preferredRow_,
                    static_cast<int>(musicalRows_.size()) - 1);
            }
        }
        else if (selectedKey_.has_value())
        {
            const auto found = std::ranges::find_if(events, [&](const talas::project::TimelineEventValue& event) {
                return event.eventName == selectedKey_->eventName
                    && event.timeSeconds == selectedKey_->timeSeconds;
            });
            if (found != events.end())
                targetRow = static_cast<int>(std::distance(events.begin(), found));
        }
        if (contentMode_ == ContentMode::Events
            && targetRow < 0 && preferredRow_ >= 0 && !events.empty())
            targetRow = std::min(preferredRow_, static_cast<int>(events.size()) - 1);

        list_.updateContent();
        if (targetRow >= 0)
        {
            list_.selectRow(targetRow);
            populateForm(targetRow);
        }
        else
        {
            list_.deselectAllRows();
            selectedKey_.reset();
            clearForm();
        }
        updateActionState();
        repaint();
    }

    void setTransportSnapshot(
        const talas::runtime::RealtimeMusicalTransportSnapshot& snapshot)
        override
    {
        latestTransportSnapshot_ = snapshot;
        auto scoreRuntime = scoreRuntimeState(
            snapshot,
            externalTransportDetail_,
            bridge_.editor().document(),
            outputMeters_ ? outputMeters_()
                : TimelineOutputMeterSnapshot{});
        const auto nextSampleRate = std::isfinite(
                snapshot.transport.sampleRate)
                && snapshot.transport.sampleRate > 0.0
            ? snapshot.transport.sampleRate
            : 48000.0;
        if (nextSampleRate != playbackSampleRate_)
        {
            scoreViewport_.rebuild(
                bridge_.editor().document(),
                std::move(scoreRuntime));
        }
        else
        {
            scoreViewport_.updateRuntime(std::move(scoreRuntime));
        }
        const auto nextTime = snapshot.transport.sampleRate > 0.0
            ? static_cast<double>(snapshot.transport.playheadFrame)
                / snapshot.transport.sampleRate
            : 0.0;
        const auto nextSmptePosition = smptePositionText(snapshot);
        if (nextTime == transportTimeSeconds_
            && nextSampleRate == playbackSampleRate_
            && snapshot.musicalPosition
                == transportMusicalPosition_
            && nextSmptePosition == transportSmptePosition_)
        {
            return;
        }
        transportTimeSeconds_ = nextTime;
        playbackSampleRate_ = nextSampleRate;
        transportMusicalPosition_ = snapshot.musicalPosition;
        transportSmptePosition_ = nextSmptePosition;
        repaint();
    }

    void setExternalTransportAccessibilityDetail(
        const juce::String& detail) override
    {
        externalTransportDetail_ = detail;
        scoreViewport_.setExternalDetail(detail);
        const auto title = detail.isNotEmpty()
            ? "Timeline. " + detail
            : juce::String{"Timeline"};
        if (getTitle() == title)
            return;
        setTitle(title);
        list_.setTitle(title);
    }

    void focusTimeline() override
    {
        if (contentMode_ == ContentMode::Score)
        {
            if (scoreViewport_.isShowing())
                scoreViewport_.grabKeyboardFocus();
            return;
        }
        if (getNumRows() > 0 && list_.getSelectedRow() < 0)
            list_.selectRow(0);
        if (list_.isShowing())
            list_.grabKeyboardFocus();
    }

    bool jumpToMarkerForProgramChange(std::uint8_t channel, std::uint8_t program) override
    {
        return scoreViewport_.takeProgramChange(channel, program);
    }

    bool markClip(const std::string& clipId, bool toggle) override
    {
        return scoreViewport_.markClip(clipId, toggle);
    }

    [[nodiscard]] talas::app::TimelineMarks& marks() noexcept override
    {
        return scoreViewport_.marks();
    }

    void setSoundPasteHandler(std::function<std::optional<std::string>()> handler) override
    {
        scoreViewport_.soundToPaste_ = std::move(handler);
    }

    void forgetCopiedClips() override
    {
        scoreViewport_.forgetCopiedClips();
    }

    void setSoundCopyHandler(std::function<void(std::vector<std::string>)> handler) override
    {
        scoreViewport_.soundsCopied_ = std::move(handler);
    }

    void setSoundEditHandler(std::function<void(const std::string&)> handler) override
    {
        scoreViewport_.soundToEdit_ = std::move(handler);
    }

    void setPasteSources(const talas::core::NodeRegistry& registry,
        std::function<std::optional<std::filesystem::path>()> projectFile) override
    {
        scoreViewport_.setPasteSources(registry, std::move(projectFile));
    }

#if defined(TALAS_WORKBENCH_TIMELINE_TEST_SUPPORT)
    [[nodiscard]] std::string infoFieldTextForSmoke(TimelineScoreSelection selection, TimelineInfoField field) override
    {
        return scoreViewport_.infoFieldTextForSmoke(std::move(selection), field);
    }

    [[nodiscard]] bool enterInfoFieldForSmoke(
        TimelineScoreSelection selection, TimelineInfoField field, const std::string& text) override
    {
        return scoreViewport_.enterInfoFieldForSmoke(std::move(selection), field, text);
    }

    [[nodiscard]] std::optional<TimelineInfoField> typingInfoFieldForSmoke() const override
    {
        return scoreViewport_.typingInfoFieldForSmoke();
    }

    [[nodiscard]] std::string typeIntoInfoFieldForSmoke(
        TimelineScoreSelection selection, TimelineInfoField field, const juce::String& keys) override
    {
        return scoreViewport_.typeIntoInfoFieldForSmoke(std::move(selection), field, keys);
    }

    [[nodiscard]] bool setMarkerProgramForSmoke(TimelineScoreSelection selection, std::uint8_t program) override
    {
        return scoreViewport_.setMarkerProgramForSmoke(std::move(selection), program);
    }

    [[nodiscard]] bool setMarkerProgramChannelForSmoke(std::uint8_t channel) override
    {
        return scoreViewport_.setMarkerProgramChannelForSmoke(channel);
    }

    [[nodiscard]] bool anchorAtCursorForSmoke(TimelineScoreSelection selection) override
    {
        return scoreViewport_.anchorAtCursorForSmoke(std::move(selection));
    }

    [[nodiscard]] bool editClipSoundForSmoke(std::string_view clipId) override
    {
        return scoreViewport_.editClipSound(clipId);
    }

    [[nodiscard]] bool makeSoundOfTimelineForSmoke() override
    {
        return scoreViewport_.makeSoundOfTimeline();
    }

    [[nodiscard]] bool copyAndPasteClipsForSmoke(
        const std::vector<std::string>& clipIds, const std::string& trackId, std::uint64_t frame) override
    {
        return scoreViewport_.copyAndPasteClipsForSmoke(clipIds, trackId, frame);
    }

    [[nodiscard]] bool zoomLaneValuesForSmoke(const std::string& laneId, int steps) override
    {
        const auto before = scoreViewport_.laneValueZoom(laneId);
        return scoreViewport_.zoomLaneValues(laneId, steps) && scoreViewport_.laneValueZoom(laneId) == before + steps;
    }

    [[nodiscard]] bool captureMarkerForSmoke() override
    {
        return scoreViewport_.captureMarkerForSmoke();
    }

    [[nodiscard]] bool alignClipForSmoke(
        TimelineScoreSelection selection, std::string_view anchoredClipId, bool end) override
    {
        return scoreViewport_.alignClipForSmoke(std::move(selection), anchoredClipId, end);
    }

    [[nodiscard]] bool dropSoundOnTrackForSmoke(
        std::string_view definitionId,
        std::string_view trackId,
        std::uint64_t frame) override
    {
        return scoreViewport_.dropSoundOnTrackForSmoke(definitionId, trackId, frame);
    }

    [[nodiscard]] bool dropSoundBelowTracksForSmoke(std::string_view definitionId, std::uint64_t frame) override
    {
        return scoreViewport_.dropSoundBelowTracksForSmoke(definitionId, frame);
    }

    [[nodiscard]] bool dropDescriptionOnTrackForSmoke(
        const juce::String& description, std::string_view trackId, std::uint64_t frame) override
    {
        return scoreViewport_.dropDescriptionOnTrackForSmoke(description, trackId, frame);
    }

    [[nodiscard]] bool applyEffectToClipForSmoke(std::string_view clipId, std::string_view typeId) override
    {
        return scoreViewport_.applyEffectToClipForSmoke(clipId, typeId);
    }

    [[nodiscard]] bool replaceClipInputForSmoke(std::string_view clipId, std::string_view definitionId) override
    {
        return scoreViewport_.replaceClipInputForSmoke(clipId, definitionId);
    }

    [[nodiscard]] bool removeClipEffectForSmoke(std::string_view clipId) override
    {
        return scoreViewport_.removeClipEffectForSmoke(clipId);
    }

    [[nodiscard]] bool setTrackInputForSmoke(std::string_view trackId, std::string_view inputTrackId) override
    {
        return scoreViewport_.setTrackInput(trackId, inputTrackId);
    }

    [[nodiscard]] std::vector<juce::String> trackInputMenuForSmoke(std::string_view trackId) override
    {
        return scoreViewport_.trackInputMenuForSmoke(trackId);
    }

    [[nodiscard]] bool setTrackSendForSmoke(std::string_view trackId, std::string_view submixId, bool connected) override
    {
        return scoreViewport_.setTrackSend(trackId, submixId, connected);
    }

    [[nodiscard]] bool addSubmixForSmoke(std::string_view trackId, std::string_view name) override
    {
        return scoreViewport_.addSubmixForTrack(trackId, name);
    }

    [[nodiscard]] bool toggleTrackOutputForSmoke(std::string_view trackId, std::uint32_t channel) override
    {
        return scoreViewport_.toggleTrackOutput(trackId, channel);
    }

    [[nodiscard]] bool setTrackMidiChannelForSmoke(std::string_view trackId, std::uint8_t channel) override
    {
        return scoreViewport_.setTrackMidiChannel(trackId, channel);
    }

    [[nodiscard]] bool setTrackHardwareInputForSmoke(std::string_view trackId, std::uint32_t channel) override
    {
        return scoreViewport_.setTrackHardwareInput(trackId, channel);
    }

    [[nodiscard]] bool setTrackNoOutputForSmoke(std::string_view trackId) override
    {
        return scoreViewport_.setTrackNoOutput(trackId);
    }

    [[nodiscard]] bool selectPointsForSmoke(std::string_view laneId, std::vector<std::size_t> indices) override
    {
        return scoreViewport_.selectPointsForSmoke(laneId, std::move(indices));
    }

    [[nodiscard]] bool boxLaneForSmoke(std::string_view laneId) override
    {
        return scoreViewport_.boxLaneForSmoke(laneId);
    }

    [[nodiscard]] bool keyForSmoke(const juce::KeyPress& key) override
    {
        const auto handled = scoreViewport_.keyPressed(key);
        (void) scoreViewport_.keyStateChanged(false);
        return handled;
    }

    [[nodiscard]] bool setSelectedPointsValueForSmoke(const std::string& text) override
    {
        return scoreViewport_.setSelectedPointsValue(text);
    }

    [[nodiscard]] bool addPointForSmoke(std::string_view laneId, std::uint64_t frame, double height) override
    {
        return scoreViewport_.addPointForSmoke(laneId, frame, height);
    }

    [[nodiscard]] std::vector<std::size_t> selectedPointsForSmoke() override
    {
        return scoreViewport_.selectedPointsForSmoke();
    }

    [[nodiscard]] bool setGridForSmoke(talas::app::TimelineGridMode mode, talas::app::TimelineQuantization quantization) override
    {
        return scoreViewport_.setGridMode(mode) && scoreViewport_.setQuantization(quantization);
    }

    void selectForSmoke(TimelineScoreSelection selection) override
    {
        scoreViewport_.selectForSmoke(std::move(selection));
    }

    [[nodiscard]] std::uint32_t sampleRateForSmoke() override
    {
        return scoreViewport_.sampleRateForSmoke();
    }

    [[nodiscard]] bool newTakeForSmoke() override
    {
        return scoreViewport_.newSelectedLaneTake();
    }

    [[nodiscard]] bool chooseTakeForSmoke(const std::string& name) override
    {
        return scoreViewport_.chooseSelectedLaneTake(name);
    }

    [[nodiscard]] bool renameTakeForSmoke(const std::string& name) override
    {
        return scoreViewport_.renameSelectedLaneTake(name);
    }

    void setEnvelopeExtractor(EnvelopeExtractor extractor) override
    {
        scoreViewport_.setEnvelopeExtractor(std::move(extractor));
    }

    [[nodiscard]] bool extractEnvelopeForSmoke(
        std::string_view laneId, const talas::runtime::EnvelopeExtractionRequest& request) override
    {
        return scoreViewport_.extractEnvelopeIntoLane(laneId, request);
    }

    [[nodiscard]] bool shiftClickClipForSmoke(const std::string& clipId) override
    {
        return scoreViewport_.shiftClickClipForSmoke(clipId);
    }

    void setTimelineRecorder(TimelineRecorder recorder) override
    {
        scoreViewport_.setTimelineRecorder(std::move(recorder));
    }

    void setTimeSourceChooser(TimeSourceChooser chooser) override
    {
        scoreViewport_.setTimeSourceChooser(std::move(chooser));
    }

    void setDragImporter(DragImporter importer) override
    {
        scoreViewport_.setDragImporter(std::move(importer));
    }

    void setSpaceKey(SpaceKey key) override
    {
        scoreViewport_.setSpaceKey(std::move(key));
    }

    [[nodiscard]] bool setTimeCodeOffsetForSmoke(std::string_view text) override
    {
        return scoreViewport_.setTimeCodeOffset(text);
    }

    [[nodiscard]] talas::audio::TimelineRecordingResult recordTimelineForSmoke(
        talas::audio::TimelineRecordingOptions options) override
    {
        return scoreViewport_.recordTimelineNow(std::move(options));
    }

    [[nodiscard]] std::vector<std::string> selectedClipsForSmoke() override
    {
        return scoreViewport_.selectedClipsForSmoke();
    }

    [[nodiscard]] bool slaveLaneForSmoke(const std::string& masterLaneId) override
    {
        return scoreViewport_.slaveSelectedLane(masterLaneId);
    }

    [[nodiscard]] bool newMasterControlForSmoke(const std::string& name) override
    {
        return scoreViewport_.addMasterControl(name);
    }

    [[nodiscard]] bool drawAutomationStrokeForSmoke(
        std::string_view laneId, std::vector<std::pair<std::uint64_t, double>> stroke) override
    {
        return scoreViewport_.drawAutomationStrokeForSmoke(laneId, std::move(stroke));
    }

    [[nodiscard]] std::vector<juce::String> automateMenuForSmoke(
        TimelineScoreSelection selection) override
    {
        return scoreViewport_.automateMenuForSmoke(std::move(selection));
    }

    [[nodiscard]] bool automateForSmoke(
        TimelineScoreSelection selection,
        std::string_view eventName) override
    {
        return scoreViewport_.automateForSmoke(std::move(selection), eventName);
    }

    [[nodiscard]] bool shapeAutomationForSmoke(
        TimelineScoreSelection selection,
        talas::app::AutomationCurveShape shape) override
    {
        return scoreViewport_.shapeAutomationForSmoke(std::move(selection), shape);
    }

    [[nodiscard]] bool transformAutomationForSmoke(
        TimelineScoreSelection selection,
        talas::app::AutomationTransform transform,
        talas::app::AutomationTransformParameters parameters) override
    {
        return scoreViewport_.transformAutomationForSmoke(
            std::move(selection), transform, parameters);
    }

    [[nodiscard]] bool setControllerRecordingTakeForSmoke(
        talas::project::ControllerRecordingTake take) override
    {
        return scoreViewport_.setControllerRecordingTakeForSmoke(
            std::move(take));
    }

    [[nodiscard]] bool invokeLaneStateMenuActionForSmoke(
        TimelineScoreSelection selection, int actionId) override
    {
        return scoreViewport_.invokeLaneStateMenuActionForSmoke(std::move(selection), actionId);
    }

    [[nodiscard]] TimelineControllerRecordingMenuState
    controllerRecordingMenuStateForSmoke() override
    {
        return scoreViewport_.controllerRecordingMenuStateForSmoke();
    }

    [[nodiscard]] bool invokeControllerRecordingMenuActionForSmoke(
        int actionId) override
    {
        return scoreViewport_.invokeControllerRecordingMenuActionForSmoke(
            actionId);
    }

    [[nodiscard]] TimelineScoreClipArtifactState
    clipArtifactStateForSmoke(std::string_view clipId) override
    {
        return scoreViewport_.clipArtifactStateForSmoke(clipId);
    }

    [[nodiscard]] bool invokeClipArtifactActionForSmoke(
        std::string clipId,
        TimelineScoreClipArtifactAction action) override
    {
        return scoreViewport_.invokeClipArtifactActionForSmoke(
            std::move(clipId), action);
    }

    [[nodiscard]] talas::app::TimelineCacheLook cacheLookForSmoke(std::string_view clipId) override
    {
        return scoreViewport_.cacheLookForSmoke(clipId);
    }

    [[nodiscard]] juce::Colour cacheMenuColourForSmoke() override
    {
        return scoreViewport_.cacheMenuColourForSmoke();
    }
#endif

    [[nodiscard]] std::optional<talas::project::MusicalPosition>
    currentMusicalPosition() const
    {
        return transportMusicalPosition_;
    }

    void paint(juce::Graphics& graphics) override
    {
        WorkbenchStyle::paintPanel(graphics, getLocalBounds().toFloat(), WorkbenchStyle::panelRadius());
        auto header = getLocalBounds().reduced(12).removeFromTop(22);
        WorkbenchStyle::paintPanelTitle(graphics, header.removeFromLeft(74), "Timeline");
        header.removeFromRight(std::min(136, header.getWidth()));
        graphics.setColour(secondaryTextColour);
        graphics.setFont(WorkbenchType::micro());
        if (getWidth() >= 470)
        {
            const auto musicalPosition = currentMusicalPosition();
            graphics.drawText(
                transportSmptePosition_.isNotEmpty()
                    ? transportSmptePosition_
                    : musicalPosition.has_value()
                    ? juce::String("Bar ")
                        + unsignedText(musicalPosition->bar)
                        + "."
                        + juce::String(musicalPosition->beat)
                        + "."
                        + juce::String(musicalPosition->tick)
                    : juceString("Musical position unavailable"),
                header,
                juce::Justification::centredRight);
        }
        if (contentMode_ != ContentMode::Score)
            drawTrack(graphics);
    }

    void resized() override
    {
        viewMode_.setBounds(
            std::max(82, getWidth() - 140),
            8,
            std::min(128, std::max(0, getWidth() - 90)),
            22);
        auto bounds = getLocalBounds().reduced(8);
        bounds.removeFromTop(24);
        if (contentMode_ == ContentMode::Score)
        {
            const auto split = talas::app::timelineControlsSplit(bounds.getHeight(), controlsShare_, 60);
            scoreViewport_.setBounds(bounds.removeFromTop(split.scoreHeight));
            controlsEditor_.setBounds(bounds);
            return;
        }
        bounds.removeFromTop(timelineTrackHeightForCurrentSize());
        bounds.removeFromTop(4);

        auto loopArea = bounds.removeFromTop(38);
        auto loopLabels = loopArea.removeFromTop(12);
        auto loopRow = loopArea.removeFromBottom(24);
        loopButton_.setBounds(loopRow.removeFromLeft(54));
        loopLabels.removeFromLeft(58);
        loopRow.removeFromLeft(4);
        loopStartLabel_.setBounds(loopLabels.removeFromLeft(72));
        loopLabels.removeFromLeft(4);
        loopEndLabel_.setBounds(loopLabels.removeFromLeft(72));
        loopStartInput_.setBounds(loopRow.removeFromLeft(72));
        loopRow.removeFromLeft(4);
        loopEndInput_.setBounds(loopRow.removeFromLeft(72));
        loopRow.removeFromLeft(4);
        loopApplyButton_.setBounds(loopRow);
        bounds.removeFromTop(5);

        auto firstArea = bounds.removeFromTop(38);
        auto firstLabels = firstArea.removeFromTop(12);
        auto firstRow = firstArea.removeFromBottom(24);
        if (contentMode_ == ContentMode::Events)
        {
            timeLabel_.setBounds(firstLabels.removeFromLeft(98));
            firstLabels.removeFromLeft(5);
            nameLabel_.setBounds(firstLabels);
            timeInput_.setBounds(firstRow.removeFromLeft(98));
            firstRow.removeFromLeft(5);
            nameInput_.setBounds(firstRow);
        }
        else
        {
            musicalKindLabel_.setBounds(firstLabels.removeFromLeft(98));
            firstLabels.removeFromLeft(5);
            musicalTickLabel_.setBounds(firstLabels);
            musicalKindInput_.setBounds(firstRow.removeFromLeft(98));
            firstRow.removeFromLeft(5);
            musicalTickInput_.setBounds(firstRow);
        }
        bounds.removeFromTop(5);

        auto secondArea = bounds.removeFromTop(38);
        auto secondLabels = secondArea.removeFromTop(12);
        auto secondRow = secondArea.removeFromBottom(24);
        if (contentMode_ == ContentMode::Events)
        {
            valueLabel_.setBounds(secondLabels.removeFromLeft(78));
            secondLabels.removeFromLeft(5);
            domainLabel_.setBounds(secondLabels.removeFromLeft(88));
            secondLabels.removeFromLeft(5);
            interpolationLabel_.setBounds(secondLabels);
            valueInput_.setBounds(secondRow.removeFromLeft(78));
            secondRow.removeFromLeft(5);
            domainInput_.setBounds(secondRow.removeFromLeft(88));
            secondRow.removeFromLeft(5);
            interpolationInput_.setBounds(secondRow);
        }
        else
        {
            musicalPrimaryLabel_.setBounds(secondLabels.removeFromLeft(108));
            secondLabels.removeFromLeft(5);
            musicalSecondaryLabel_.setBounds(secondLabels);
            musicalPrimaryInput_.setBounds(secondRow.removeFromLeft(108));
            secondRow.removeFromLeft(5);
            musicalSecondaryInput_.setBounds(secondRow);
        }
        bounds.removeFromTop(5);

        auto buttons = bounds.removeFromTop(24);
        addButton_.setBounds(buttons.removeFromLeft(58));
        buttons.removeFromLeft(5);
        updateButton_.setBounds(buttons.removeFromLeft(70));
        buttons.removeFromLeft(5);
        removeButton_.setBounds(buttons.removeFromLeft(92));
        bounds.removeFromTop(6);
        list_.setBounds(bounds);

        updateLaneScrollBar();
        auto scrollBounds =
            trackBounds();
        laneScrollBar_.setBounds(
            scrollBounds.removeFromRight(
                timelineLaneScrollBarWidth)
                .reduced(1));
    }

    void mouseWheelMove(
        const juce::MouseEvent& event,
        const juce::MouseWheelDetails&
            wheel) override
    {
        if (contentMode_ == ContentMode::Score)
        {
            juce::Component::mouseWheelMove(event, wheel);
            return;
        }
        if (trackBounds().contains(
                event.getPosition())
            && laneScrollBar_.isVisible()
            && wheel.deltaY != 0.0F)
        {
            laneScrollBar_.moveScrollbarInSteps(
                wheel.deltaY > 0.0F
                    ? -3
                    : 3);
            return;
        }
        juce::Component::mouseWheelMove(
            event,
            wheel);
    }

    void mouseDown(const juce::MouseEvent& event) override
    {
        if (contentMode_ == ContentMode::Score)
            return;
        pressedMarker_.reset();
        pressedLoopBoundary_.reset();
        dragChanged_ = false;
        const auto track = trackBounds();
        if (!track.contains(event.getPosition()))
            return;
        const auto timeTrack =
            timeTrackBounds();
        const auto& timeline = bridge_.editor().document().timeline;
        const auto duration = trackDuration();
        if (timeline.loop.enabled)
        {
            const auto trackWidth =
                static_cast<float>(std::max(0, timeTrack.getWidth() - 1));
            const auto startX = timeTrack.getX()
                + static_cast<float>(
                    timeline.loop.startSeconds / duration)
                    * trackWidth;
            const auto endX = timeTrack.getX()
                + static_cast<float>(
                    timeline.loop.endSeconds / duration)
                    * trackWidth;
            const auto startDistance =
                std::abs(event.position.x - startX);
            const auto endDistance =
                std::abs(event.position.x - endX);
            if (std::min(startDistance, endDistance) <= 12.0F)
            {
                pressedLoopBoundary_ =
                    startDistance <= endDistance
                    ? talas::project::TimelineLoopBoundary::Start
                    : talas::project::TimelineLoopBoundary::End;
                dragVisibleDurationSeconds_ = duration;
                dragTrackWidthPixels_ = static_cast<double>(
                    std::max(1, timeTrack.getWidth() - 1));
                return;
            }
        }
        const auto& events = bridge_.editor().document().timeline.eventValues;
        if (events.empty())
            return;
        const auto laneIndex =
            talas::project::timelineLaneAtPosition(
                static_cast<double>(
                    event.position.y
                    - track.getY())
                    + laneScrollOffset(),
                laneContentHeight(),
                lanes_.size());
        if (!laneIndex.has_value())
            return;

        auto nearestRow = -1;
        auto nearestDistance = 12.0F;
        for (const auto index :
             lanes_[*laneIndex].eventIndices)
        {
            const auto x = timeTrack.getX()
                + static_cast<float>(events[index].timeSeconds / duration)
                    * static_cast<float>(
                        std::max(
                            0,
                            timeTrack.getWidth()
                                - 1));
            const auto distance = std::abs(event.position.x - x);
            if (distance <= nearestDistance)
            {
                nearestDistance = distance;
                nearestRow = static_cast<int>(index);
            }
        }
        if (nearestRow >= 0)
        {
            list_.selectRow(nearestRow);
            pressedMarker_ =
                events[static_cast<std::size_t>(
                    nearestRow)];
            dragVisibleDurationSeconds_ = duration;
            dragTrackWidthPixels_ =
                static_cast<double>(
                    std::max(
                        1,
                        timeTrack.getWidth()
                            - 1));
        }
    }

    void mouseDrag(const juce::MouseEvent& event) override
    {
        if (contentMode_ == ContentMode::Score)
            return;
        if (pressedLoopBoundary_.has_value())
        {
            dragLoopBoundary(event);
            return;
        }
        if (!pressedMarker_.has_value()
            || !event.mouseWasDraggedSinceMouseDown())
        {
            return;
        }
        if (!dragTransaction_.has_value())
            dragTransaction_ = bridge_.editor().beginOwnedHistoryTransaction();

        const auto& events =
            bridge_.editor().document().timeline.eventValues;
        const auto found = std::ranges::find_if(
            events,
            [&](const talas::project::TimelineEventValue& candidate) {
                return candidate.eventName
                        == pressedMarker_->eventName
                    && candidate.timeSeconds
                        == pressedMarker_->timeSeconds;
            });
        if (found == events.end())
            return;

        const auto track =
            timeTrackBounds();
        const auto mapped = talas::project::mapTimelineMarkerDrag(
            static_cast<double>(event.position.x)
                - static_cast<double>(track.getX()),
            dragTrackWidthPixels_,
            dragVisibleDurationSeconds_,
            !event.mods.isShiftDown());
        if (mapped.timeSeconds == pressedMarker_->timeSeconds)
            return;

        auto moved = *pressedMarker_;
        moved.timeSeconds = mapped.timeSeconds;
        const auto row = static_cast<std::size_t>(
            std::distance(events.begin(), found));
        const auto result =
            bridge_.editor().updateTimelineEventValue(
                row, moved, dragTransaction_.value_or(0));
        if (!result.ok())
        {
            onStatus_(juceString(result.message));
            return;
        }

        pressedMarker_ = moved;
        selectedKey_ = moved;
        preferredRow_ = -1;
        dragChanged_ = true;
        refresh();
        onStatus_(
            "Timeline marker "
            + formatTime(moved.timeSeconds)
            + (event.mods.isShiftDown()
                    ? juceString(" · free")
                    : juceString(" · snapped")));
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        if (contentMode_ == ContentMode::Score)
            return;
        finishMarkerDrag();
    }

private:
    void scrollBarMoved(
        juce::ScrollBar* scrollBar,
        double) override
    {
        if (scrollBar == &laneScrollBar_)
            repaint(trackBounds());
    }

    [[nodiscard]] double
    laneContentHeight() const
    {
        return talas::project::
            timelineLaneContentHeight(
                static_cast<double>(
                    trackBounds()
                        .getHeight()),
                lanes_.size(),
                minimumTimelineLaneHeight);
    }

    [[nodiscard]] double
    laneScrollOffset() const
    {
        return laneScrollBar_.isVisible()
            ? laneScrollBar_
                  .getCurrentRangeStart()
            : 0.0;
    }

    void updateLaneScrollBar()
    {
        const auto viewportHeight =
            static_cast<double>(
                trackBounds().getHeight());
        if (viewportHeight <= 0.0)
        {
            laneScrollBar_.setVisible(false);
            return;
        }
        const auto contentHeight =
            laneContentHeight();
        const auto needsScroll =
            contentHeight > viewportHeight;
        const auto start = std::clamp(
            laneScrollBar_
                .getCurrentRangeStart(),
            0.0,
            std::max(
                0.0,
                contentHeight
                    - viewportHeight));
        laneScrollBar_.setRangeLimits(
            0.0,
            contentHeight,
            juce::dontSendNotification);
        laneScrollBar_.setCurrentRange(
            needsScroll ? start : 0.0,
            viewportHeight,
            juce::dontSendNotification);
        laneScrollBar_.setVisible(
            needsScroll);
        repaint(trackBounds());
    }

    void revealLaneForEvent(
        std::size_t eventIndex)
    {
        if (!laneScrollBar_.isVisible()
            || lanes_.empty())
        {
            return;
        }
        const auto lane =
            std::ranges::find_if(
                lanes_,
                [&](const auto& candidate) {
                    return std::ranges::find(
                               candidate.eventIndices,
                               eventIndex)
                        != candidate
                               .eventIndices.end();
                });
        if (lane == lanes_.end())
            return;

        const auto laneIndex =
            static_cast<std::size_t>(
                std::distance(
                    lanes_.begin(),
                    lane));
        const auto contentHeight =
            laneContentHeight();
        const auto laneHeight =
            contentHeight
            / static_cast<double>(
                lanes_.size());
        const auto laneTop =
            static_cast<double>(
                laneIndex)
            * laneHeight;
        const auto laneBottom =
            laneTop + laneHeight;
        const auto viewportHeight =
            static_cast<double>(
                trackBounds().getHeight());
        const auto currentStart =
            laneScrollOffset();
        if (laneTop < currentStart)
        {
            laneScrollBar_
                .setCurrentRangeStart(
                    laneTop);
        }
        else if (
            laneBottom
            > currentStart
                + viewportHeight)
        {
            laneScrollBar_
                .setCurrentRangeStart(
                    laneBottom
                        - viewportHeight);
        }
    }

    void rebuildMusicalRows()
    {
        musicalRows_.clear();
        const auto& musical =
            bridge_.editor().document().timeline.musicalTime;
        musicalRows_.reserve(
            musical.tempo.size() + musical.meter.size());
        for (std::size_t index = 0; index < musical.tempo.size(); ++index)
            musicalRows_.push_back({true, index, musical.tempo[index].tick});
        for (std::size_t index = 0; index < musical.meter.size(); ++index)
            musicalRows_.push_back({false, index, musical.meter[index].tick});
        std::ranges::sort(
            musicalRows_,
            [](const MusicalRow& left, const MusicalRow& right) {
                if (left.tick != right.tick)
                    return left.tick < right.tick;
                return left.tempo && !right.tempo;
            });
    }

    [[nodiscard]] juce::String musicalLocationText(
        std::uint64_t tick) const
    {
        const auto position = talas::project::musicalTickToPosition(
            bridge_.editor().document().timeline.musicalTime,
            tick);
        if (!position.has_value())
            return "unavailable";
        return unsignedText(position->bar)
            + "." + juce::String(position->beat)
            + "." + juce::String(position->tick);
    }

    [[nodiscard]] juce::String musicalRowAccessibilityText(int row) const
    {
        if (row < 0 || row >= static_cast<int>(musicalRows_.size()))
            return {};
        const auto& source = musicalRows_[static_cast<std::size_t>(row)];
        const auto& musical =
            bridge_.editor().document().timeline.musicalTime;
        if (source.tempo)
        {
            const auto& event = musical.tempo[source.sourceIndex];
            const auto bpm = 60000000.0
                / static_cast<double>(event.microsecondsPerQuarter);
            return "Tempo at bar beat tick " + musicalLocationText(event.tick)
                + ", " + juce::String(bpm, 6) + " BPM, exact "
                + juce::String(event.microsecondsPerQuarter)
                + " microseconds per quarter note";
        }
        const auto& event = musical.meter[source.sourceIndex];
        return "Meter at bar beat tick " + musicalLocationText(event.tick)
            + ", " + juce::String(event.numerator)
            + " over " + juce::String(event.denominator);
    }

    void paintMusicalRow(
        int row,
        juce::Graphics& graphics,
        int width,
        int height,
        bool selected)
    {
        if (row < 0 || row >= static_cast<int>(musicalRows_.size()))
            return;
        if (selected)
        {
            graphics.setColour(workbenchTheme.selectedRow);
            graphics.fillRoundedRectangle(juce::Rectangle<float>{2.0F, 1.0F, static_cast<float>(width) - 4.0F, static_cast<float>(height) - 2.0F}, 5.0F);
        }
        const auto& source = musicalRows_[static_cast<std::size_t>(row)];
        const auto& musical =
            bridge_.editor().document().timeline.musicalTime;
        auto bounds = juce::Rectangle<int>{0, 0, width, height}.reduced(6, 2);
        graphics.setColour(secondaryTextColour);
        graphics.setFont(WorkbenchType::micro());
        graphics.drawText(
            musicalLocationText(source.tick),
            bounds.removeFromLeft(72),
            juce::Justification::centredLeft);
        graphics.setColour(source.tempo ? controlColour : eventColour);
        graphics.setFont(WorkbenchType::bodyStrong());
        graphics.drawText(
            source.tempo ? "Tempo" : "Meter",
            bounds.removeFromLeft(64),
            juce::Justification::centredLeft);
        graphics.setColour(textColour);
        graphics.setFont(WorkbenchType::label());
        if (source.tempo)
        {
            const auto& event = musical.tempo[source.sourceIndex];
            graphics.drawText(
                juce::String(
                    60000000.0
                        / static_cast<double>(
                            event.microsecondsPerQuarter),
                    6)
                    + juceString(" BPM · tick ")
                    + unsignedText(event.tick),
                bounds,
                juce::Justification::centredRight);
        }
        else
        {
            const auto& event = musical.meter[source.sourceIndex];
            graphics.drawText(
                juce::String(event.numerator) + "/"
                    + juce::String(event.denominator)
                    + juceString(" · tick ")
                    + unsignedText(event.tick),
                bounds,
                juce::Justification::centredRight);
        }
    }

    void populateMusicalForm(int row)
    {
        if (row < 0 || row >= static_cast<int>(musicalRows_.size()))
        {
            preferredRow_ = -1;
            updateActionState();
            return;
        }
        const auto& source = musicalRows_[static_cast<std::size_t>(row)];
        const auto& musical =
            bridge_.editor().document().timeline.musicalTime;
        preferredRow_ = row;
        musicalKindInput_.setSelectedId(
            source.tempo ? 1 : 2,
            juce::dontSendNotification);
        musicalTickInput_.setText(
            unsignedText(source.tick),
            juce::dontSendNotification);
        if (source.tempo)
        {
            const auto& event = musical.tempo[source.sourceIndex];
            musicalPrimaryInput_.setText(
                formatEditableDouble(
                    60000000.0
                        / static_cast<double>(
                            event.microsecondsPerQuarter)),
                juce::dontSendNotification);
        }
        else
        {
            const auto& event = musical.meter[source.sourceIndex];
            musicalPrimaryInput_.setText(
                juce::String(event.numerator),
                juce::dontSendNotification);
            musicalSecondaryInput_.setText(
                juce::String(event.denominator),
                juce::dontSendNotification);
        }
        updateMusicalFormLabels();
        updateActionState();
    }

    [[nodiscard]] static std::optional<std::uint64_t>
    parseUnsignedInteger(const juce::String& text)
    {
        const auto source = text.trim().toStdString();
        if (source.empty())
            return std::nullopt;
        std::uint64_t value = 0;
        const auto [end, error] = std::from_chars(
            source.data(), source.data() + source.size(), value);
        if (error != std::errc{} || end != source.data() + source.size())
            return std::nullopt;
        return value;
    }

    [[nodiscard]] std::optional<talas::project::MusicalTempoEvent>
    tempoEventFromForm()
    {
        const auto tick = parseUnsignedInteger(musicalTickInput_.getText());
        const auto bpm = parseFiniteDouble(musicalPrimaryInput_.getText());
        if (!tick.has_value())
        {
            onStatus_("Musical tick must be an exact non-negative integer");
            return std::nullopt;
        }
        if (!bpm.has_value() || *bpm <= 0.0)
        {
            onStatus_("Tempo must be a finite positive BPM value");
            return std::nullopt;
        }
        const auto exactMicroseconds = 60000000.0 / *bpm;
        if (!std::isfinite(exactMicroseconds)
            || exactMicroseconds
                > static_cast<double>(
                    std::numeric_limits<std::uint32_t>::max()))
        {
            onStatus_("Tempo is outside the supported 1 to 60000 BPM range");
            return std::nullopt;
        }
        const auto microseconds = static_cast<std::uint64_t>(
            std::llround(exactMicroseconds));
        if (microseconds < talas::project::minimumMicrosecondsPerQuarter
            || microseconds > talas::project::maximumMicrosecondsPerQuarter)
        {
            onStatus_("Tempo is outside the supported 1 to 60000 BPM range");
            return std::nullopt;
        }
        return talas::project::MusicalTempoEvent{
            *tick,
            static_cast<std::uint32_t>(microseconds),
        };
    }

    [[nodiscard]] std::optional<talas::project::MusicalMeterEvent>
    meterEventFromForm()
    {
        const auto tick = parseUnsignedInteger(musicalTickInput_.getText());
        const auto numerator = parseUnsignedInteger(
            musicalPrimaryInput_.getText());
        const auto denominator = parseUnsignedInteger(
            musicalSecondaryInput_.getText());
        if (!tick.has_value() || !numerator.has_value()
            || !denominator.has_value()
            || *numerator > std::numeric_limits<std::uint8_t>::max()
            || *denominator > std::numeric_limits<std::uint8_t>::max())
        {
            onStatus_("Meter requires an exact tick, numerator, and denominator");
            return std::nullopt;
        }
        return talas::project::MusicalMeterEvent{
            *tick,
            static_cast<std::uint8_t>(*numerator),
            static_cast<std::uint8_t>(*denominator),
        };
    }

    void addMusicalEvent()
    {
        const auto tempo = musicalKindInput_.getSelectedId() != 2;
        const auto result = tempo
            ? [this] {
                  const auto event = tempoEventFromForm();
                  return event.has_value()
                      ? bridge_.editor().addTempoEvent(*event)
                      : talas::project::EditResult{
                            talas::project::EditError::InvalidTimeline, {}};
              }()
            : [this] {
                  const auto event = meterEventFromForm();
                  return event.has_value()
                      ? bridge_.editor().addMeterEvent(*event)
                      : talas::project::EditResult{
                            talas::project::EditError::InvalidTimeline, {}};
              }();
        if (!result.ok())
        {
            if (!result.message.empty())
                onStatus_(juceString(result.message));
            return;
        }
        preferredRow_ = static_cast<int>(musicalRows_.size());
        onDocumentChange_();
        onStatus_(tempo ? "Added tempo event" : "Added meter event");
    }

    void updateMusicalEvent()
    {
        const auto row = selectedRow();
        if (row < 0 || row >= static_cast<int>(musicalRows_.size()))
        {
            onStatus_("Select a musical-map event to update");
            return;
        }
        const auto source = musicalRows_[static_cast<std::size_t>(row)];
        const auto result = source.tempo
            ? [this, source] {
                  const auto event = tempoEventFromForm();
                  return event.has_value()
                      ? bridge_.editor().updateTempoEvent(
                            source.sourceIndex, *event)
                      : talas::project::EditResult{
                            talas::project::EditError::InvalidTimeline, {}};
              }()
            : [this, source] {
                  const auto event = meterEventFromForm();
                  return event.has_value()
                      ? bridge_.editor().updateMeterEvent(
                            source.sourceIndex, *event)
                      : talas::project::EditResult{
                            talas::project::EditError::InvalidTimeline, {}};
              }();
        if (!result.ok())
        {
            if (!result.message.empty())
                onStatus_(juceString(result.message));
            return;
        }
        preferredRow_ = row;
        onDocumentChange_();
        onStatus_(source.tempo ? "Updated tempo event" : "Updated meter event");
    }

    void removeMusicalEvent()
    {
        const auto row = selectedRow();
        if (row < 0 || row >= static_cast<int>(musicalRows_.size()))
        {
            onStatus_("Select a musical-map event to delete");
            return;
        }
        const auto source = musicalRows_[static_cast<std::size_t>(row)];
        const auto result = source.tempo
            ? bridge_.editor().removeTempoEvent(source.sourceIndex)
            : bridge_.editor().removeMeterEvent(source.sourceIndex);
        if (!result.ok())
        {
            onStatus_(juceString(result.message));
            return;
        }
        preferredRow_ = row;
        onDocumentChange_();
        onStatus_(source.tempo ? "Deleted tempo event" : "Deleted meter event");
    }

    int getNumRows() override
    {
        if (contentMode_ == ContentMode::MusicalMap)
            return static_cast<int>(musicalRows_.size());
        return static_cast<int>(bridge_.editor().document().timeline.eventValues.size());
    }

    juce::String getNameForRow(int row) override
    {
        if (contentMode_ == ContentMode::MusicalMap)
            return musicalRowAccessibilityText(row);
        const auto& events = bridge_.editor().document().timeline.eventValues;
        if (row < 0 || row >= static_cast<int>(events.size()))
            return {};
        const auto& event = events[static_cast<std::size_t>(row)];
        return juceString(event.eventName)
            + " at "
            + formatTime(event.timeSeconds)
            + ", value "
            + juce::String(event.value, 4)
            + " "
            + domainName(event.domain)
            + ", "
            + interpolationName(event.interpolation);
    }

    void paintListBoxItem(int row, juce::Graphics& graphics, int width, int height, bool selected) override
    {
        if (contentMode_ == ContentMode::MusicalMap)
        {
            paintMusicalRow(row, graphics, width, height, selected);
            return;
        }
        const auto& events = bridge_.editor().document().timeline.eventValues;
        if (row < 0 || row >= static_cast<int>(events.size()))
            return;
        const auto& event = events[static_cast<std::size_t>(row)];
        if (selected)
        {
            graphics.setColour(workbenchTheme.selectedRow);
            graphics.fillRoundedRectangle(juce::Rectangle<float>{2.0F, 1.0F, static_cast<float>(width) - 4.0F, static_cast<float>(height) - 2.0F}, 5.0F);
        }

        auto bounds = juce::Rectangle<int>{0, 0, width, height}.reduced(6, 2);
        graphics.setColour(secondaryTextColour);
        graphics.setFont(WorkbenchType::micro());
        graphics.drawText(formatTime(event.timeSeconds), bounds.removeFromLeft(68),
                          juce::Justification::centredLeft);
        graphics.setColour(textColour);
        graphics.setFont(WorkbenchType::bodyStrong());
        graphics.drawText(juceString(event.eventName), bounds.removeFromLeft(std::max(70, bounds.getWidth() / 2)),
                          juce::Justification::centredLeft);
        graphics.setColour(secondaryTextColour);
        graphics.setFont(WorkbenchType::micro());
        graphics.drawText(
            juce::String(event.value, 4)
                + " "
                + domainName(event.domain)
                + juceString(" · ")
                + interpolationName(
                    event.interpolation),
            bounds,
                          juce::Justification::centredRight);
    }

    void selectedRowsChanged(int row) override
    {
        populateForm(row);
    }

    void populateForm(int row)
    {
        if (contentMode_ == ContentMode::MusicalMap)
        {
            populateMusicalForm(row);
            return;
        }
        const auto& events = bridge_.editor().document().timeline.eventValues;
        if (row < 0 || row >= static_cast<int>(events.size()))
        {
            selectedKey_.reset();
            preferredRow_ = -1;
            updateActionState();
            repaint(trackBounds());
            return;
        }

        const auto& event = events[static_cast<std::size_t>(row)];
        selectedKey_ = event;
        preferredRow_ = row;
        revealLaneForEvent(
            static_cast<std::size_t>(
                row));
        timeInput_.setText(formatEditableDouble(event.timeSeconds), juce::dontSendNotification);
        nameInput_.setText(juceString(event.eventName), juce::dontSendNotification);
        valueInput_.setText(formatEditableDouble(event.value), juce::dontSendNotification);
        domainInput_.setSelectedId(domainId(event.domain), juce::dontSendNotification);
        interpolationInput_.setSelectedId(
            interpolationId(
                event.interpolation),
            juce::dontSendNotification);
        updateActionState();
        repaint(trackBounds());
    }

    [[nodiscard]] int selectedRow() const
    {
        return list_.getSelectedRow();
    }

    static std::optional<double> parseFiniteDouble(const juce::String& text)
    {
        const auto input = text.trim().toStdString();
        if (input.empty())
            return std::nullopt;
        errno = 0;
        char* end = nullptr;
        const auto value = std::strtod(input.c_str(), &end);
        if (errno == ERANGE || end == input.c_str() || *end != '\0' || !std::isfinite(value))
            return std::nullopt;
        return value;
    }

    std::optional<talas::project::TimelineEventValue> eventFromForm()
    {
        const auto time = parseFiniteDouble(timeInput_.getText());
        const auto value = parseFiniteDouble(valueInput_.getText());
        const auto name = nameInput_.getText().trim().toStdString();
        if (!time.has_value() || *time < 0.0)
        {
            onStatus_("Timeline time must be a finite non-negative number");
            return std::nullopt;
        }
        if (!value.has_value())
        {
            onStatus_("Timeline value must be a finite number");
            return std::nullopt;
        }
        if (name.empty())
        {
            onStatus_("Timeline Event Value name cannot be empty");
            return std::nullopt;
        }
        return talas::project::TimelineEventValue{
            *time,
            name,
            selectedDomain(),
            *value,
            selectedInterpolation(),
        };
    }

    void addEvent()
    {
        auto event = eventFromForm();
        if (!event.has_value())
            return;
        const auto result = bridge_.editor().addTimelineEventValue(*event);
        if (!result.ok())
        {
            onStatus_(juceString(result.message));
            return;
        }
        selectedKey_ = std::move(event);
        preferredRow_ = -1;
        onDocumentChange_();
        onStatus_("Added Timeline Event Value");
    }

    void updateEvent()
    {
        const auto row = selectedRow();
        if (row < 0)
        {
            onStatus_("Select a Timeline Event Value to update");
            return;
        }
        auto event = eventFromForm();
        if (!event.has_value())
            return;
        const auto& existing = bridge_.editor().document().timeline.eventValues[static_cast<std::size_t>(row)];
        if (existing.timeSeconds == event->timeSeconds
            && existing.eventName == event->eventName
            && existing.domain == event->domain
            && existing.value == event->value
            && existing.interpolation
                == event->interpolation)
        {
            onStatus_("Timeline Event Value is unchanged");
            return;
        }
        const auto result = bridge_.editor().updateTimelineEventValue(static_cast<std::size_t>(row), *event);
        if (!result.ok())
        {
            onStatus_(juceString(result.message));
            return;
        }
        selectedKey_ = std::move(event);
        preferredRow_ = row;
        onDocumentChange_();
        onStatus_("Updated Timeline Event Value");
    }

    void removeEvent()
    {
        const auto row = selectedRow();
        if (row < 0)
        {
            onStatus_("Select a Timeline Event Value to delete");
            return;
        }
        const auto result = bridge_.editor().removeTimelineEventValue(static_cast<std::size_t>(row));
        if (!result.ok())
        {
            onStatus_(juceString(result.message));
            return;
        }
        selectedKey_.reset();
        preferredRow_ = row;
        onDocumentChange_();
        onStatus_("Deleted Timeline Event Value");
    }

    void applyLoop()
    {
        const auto start =
            parseFiniteDouble(loopStartInput_.getText());
        const auto end =
            parseFiniteDouble(loopEndInput_.getText());
        if (!start.has_value() || !end.has_value()
            || *start < 0.0 || *end <= *start)
        {
            onStatus_(
                "Loop requires finite bounds with end later than start");
            refresh();
            return;
        }
        const talas::project::TimelineLoopRegion loop{
            loopButton_.getToggleState(),
            *start,
            *end,
        };
        const auto result = bridge_.editor().setTimelineLoop(loop);
        if (!result.ok())
        {
            onStatus_(juceString(result.message));
            refresh();
            return;
        }
        onDocumentChange_();
        onStatus_(
            loop.enabled ? "Updated Timeline loop"
                         : "Disabled Timeline loop");
    }

    void dragLoopBoundary(const juce::MouseEvent& event)
    {
        if (!event.mouseWasDraggedSinceMouseDown())
            return;
        if (!dragTransaction_.has_value())
            dragTransaction_ = bridge_.editor().beginOwnedHistoryTransaction();

        auto loop = bridge_.editor().document().timeline.loop;
        const auto boundary = *pressedLoopBoundary_;
        const auto otherBoundary =
            boundary == talas::project::TimelineLoopBoundary::Start
            ? loop.endSeconds
            : loop.startSeconds;
        const auto track =
            timeTrackBounds();
        const auto mapped =
            talas::project::mapTimelineLoopBoundaryDrag(
                static_cast<double>(event.position.x)
                    - static_cast<double>(track.getX()),
                dragTrackWidthPixels_,
                dragVisibleDurationSeconds_,
                !event.mods.isShiftDown(),
                boundary,
                otherBoundary);
        auto& target =
            boundary == talas::project::TimelineLoopBoundary::Start
            ? loop.startSeconds
            : loop.endSeconds;
        if (target == mapped.timeSeconds)
            return;
        target = mapped.timeSeconds;
        const auto result = bridge_.editor().setTimelineLoop(
            loop, dragTransaction_.value_or(0));
        if (!result.ok())
        {
            onStatus_(juceString(result.message));
            return;
        }
        dragChanged_ = true;
        refresh();
        onStatus_(
            "Loop "
            + juce::String(
                boundary == talas::project::TimelineLoopBoundary::Start
                    ? "start "
                    : "end ")
            + formatTime(target)
            + (event.mods.isShiftDown()
                   ? juceString(" · free")
                   : juceString(" · snapped")));
    }

    void clearForm()
    {
        if (contentMode_ == ContentMode::MusicalMap)
        {
            musicalKindInput_.setSelectedId(1, juce::dontSendNotification);
            musicalKindInput_.setEnabled(true);
            musicalTickInput_.clear();
            musicalPrimaryInput_.clear();
            musicalSecondaryInput_.clear();
            updateMusicalFormLabels();
            return;
        }
        timeInput_.clear();
        nameInput_.clear();
        valueInput_.clear();
        domainInput_.setSelectedId(1, juce::dontSendNotification);
        interpolationInput_.setSelectedId(
            1,
            juce::dontSendNotification);
    }

    void updateActionState()
    {
        if (contentMode_ == ContentMode::Score)
            return;
        const auto hasSelection = selectedRow() >= 0;
        updateButton_.setEnabled(hasSelection);
        removeButton_.setEnabled(hasSelection);
        musicalKindInput_.setEnabled(
            contentMode_ != ContentMode::MusicalMap || !hasSelection);
    }

    void updateModeVisibility()
    {
        const auto events = contentMode_ == ContentMode::Events;
        const auto score = contentMode_ == ContentMode::Score;
        const std::array<juce::Component*, 10> eventComponents{
            &timeInput_, &nameInput_, &valueInput_, &domainInput_,
            &interpolationInput_, &timeLabel_, &nameLabel_,
            &valueLabel_, &domainLabel_, &interpolationLabel_,
        };
        for (auto* component : eventComponents)
            component->setVisible(events && !score);
        const std::array<juce::Component*, 8> musicalComponents{
            &musicalKindInput_, &musicalTickInput_, &musicalPrimaryInput_,
            &musicalSecondaryInput_, &musicalKindLabel_,
            &musicalTickLabel_, &musicalPrimaryLabel_,
            &musicalSecondaryLabel_,
        };
        for (auto* component : musicalComponents)
            component->setVisible(!events && !score);
        const std::array<juce::Component*, 10> legacySharedComponents{
            &list_, &laneScrollBar_, &loopButton_, &loopStartInput_,
            &loopEndInput_, &loopApplyButton_, &loopStartLabel_,
            &loopEndLabel_, &addButton_, &updateButton_,
        };
        for (auto* component : legacySharedComponents)
            component->setVisible(!score);
        removeButton_.setVisible(!score);
        scoreViewport_.setVisible(score);
        controlsEditor_.setVisible(score);
        if (score)
        {
            list_.setName("Timeline Event Values");
            resized();
            return;
        }
        addButton_.setButtonText(events ? "Add" : "Add Map Event");
        updateButton_.setButtonText(events ? "Update" : "Update Map Event");
        removeButton_.setButtonText(events ? "Delete Event" : "Delete Map Event");
        describeControl(
            addButton_,
            events ? "Add Timeline Event" : "Add Musical Map Event",
            events
                ? "Add a Timeline Event Value from the labeled fields."
                : "Add a tempo or meter event from the labeled fields.");
        describeControl(
            updateButton_,
            events ? "Update Timeline Event"
                   : "Update Musical Map Event",
            events
                ? "Apply the labeled field values to the selected Timeline event."
                : "Apply the labeled field values to the selected tempo or meter event.");
        describeControl(
            removeButton_,
            events ? "Delete Timeline Event"
                   : "Delete Musical Map Event",
            events
                ? "Delete the selected Timeline Event Value."
                : "Delete the selected tempo or meter event.");
        list_.setName(events ? "Timeline Event Values" : "Musical Map");
        resized();
    }

    void updateMusicalFormLabels()
    {
        const auto tempo = musicalKindInput_.getSelectedId() != 2;
        musicalPrimaryLabel_.setText(
            tempo ? "Tempo (BPM)" : "Numerator",
            juce::dontSendNotification);
        musicalSecondaryLabel_.setText(
            tempo ? "Canonical us/qn (auto)" : "Denominator",
            juce::dontSendNotification);
        musicalSecondaryInput_.setEnabled(!tempo);
        if (tempo)
            musicalSecondaryInput_.setText("auto", juce::dontSendNotification);
        else if (musicalSecondaryInput_.getText() == "auto")
            musicalSecondaryInput_.setText("4", juce::dontSendNotification);
    }

    void finishMarkerDrag()
    {
        const auto changed = dragChanged_;
        const auto wasLoopDrag =
            pressedLoopBoundary_.has_value();
        if (dragTransaction_.has_value())
            bridge_.editor().endOwnedHistoryTransaction(*dragTransaction_);
        pressedMarker_.reset();
        pressedLoopBoundary_.reset();
        dragTransaction_.reset();
        dragChanged_ = false;
        if (!changed)
            return;
        onDocumentChange_();
        onStatus_(
            wasLoopDrag ? "Moved Timeline loop boundary"
                        : "Moved Timeline Event Value");
    }

    [[nodiscard]] juce::Rectangle<int> trackBounds() const
    {
        auto bounds = getLocalBounds().reduced(8);
        bounds.removeFromTop(24);
        return bounds.removeFromTop(
            timelineTrackHeightForCurrentSize())
            .reduced(4, 3);
    }

    [[nodiscard]] int timelineTrackHeightForCurrentSize() const noexcept
    {
        constexpr int adaptationRange =
            regularTimelinePanelHeight
            - compactTimelinePanelHeight;
        const auto progress = juce::jlimit(
            0,
            adaptationRange,
            getHeight() - compactTimelinePanelHeight);
        return compactTimelineTrackHeight
            + ((regularTimelineTrackHeight
                - compactTimelineTrackHeight)
                   * progress
               + adaptationRange / 2)
                / adaptationRange;
    }

    [[nodiscard]] juce::Rectangle<int>
    timeTrackBounds() const
    {
        auto track = trackBounds();
        track.removeFromLeft(
            std::min(
                timelineLaneLabelWidth,
                std::max(
                    0,
                    track.getWidth() - 1)));
        if (laneScrollBar_.isVisible())
        {
            track.removeFromRight(
                std::min(
                    timelineLaneScrollBarWidth,
                    std::max(
                        0,
                        track.getWidth()
                            - 1)));
        }
        return track;
    }

    [[nodiscard]] double trackDuration() const
    {
        if (pressedMarker_.has_value()
            || pressedLoopBoundary_.has_value())
            return dragVisibleDurationSeconds_;
        const auto& events = bridge_.editor().document().timeline.eventValues;
        const auto lastEventTime = events.empty() ? 0.0 : events.back().timeSeconds;
        const auto& loop = bridge_.editor().document().timeline.loop;
        return talas::project::timelineVisibleDuration(
            std::max(
                lastEventTime,
                loop.enabled ? loop.endSeconds : 0.0),
            transportTimeSeconds_);
    }

    void drawTrack(juce::Graphics& graphics)
    {
        juce::Graphics::ScopedSaveState
            saveState(graphics);
        graphics.reduceClipRegion(
            trackBounds());
        const auto track =
            trackBounds().toFloat();
        const auto timeTrack =
            timeTrackBounds().toFloat();
        graphics.setColour(backgroundColour);
        graphics.fillRoundedRectangle(track, 4.0F);
        graphics.setColour(panelBorderColour);
        graphics.drawRoundedRectangle(track, 4.0F, 1.0F);
        graphics.drawVerticalLine(
            static_cast<int>(
                timeTrack.getX()),
            track.getY() + 1.0F,
            track.getBottom() - 1.0F);

        const auto duration = trackDuration();
        const auto& events =
            bridge_.editor().document()
                .timeline.eventValues;
        const auto& lanes = lanes_;
        const auto laneCount =
            std::max<std::size_t>(
                1,
                lanes.size());
        const auto contentHeight =
            laneContentHeight();
        const auto scrollOffset =
            laneScrollOffset();
        const auto laneHeight =
            contentHeight
            / static_cast<double>(
                laneCount);
        for (std::size_t laneIndex = 0;
             laneIndex < laneCount;
             ++laneIndex)
        {
            const auto center =
                talas::project::
                    timelineLaneVisibleCenterPosition(
                        laneIndex,
                        contentHeight,
                        laneCount,
                        scrollOffset,
                        static_cast<double>(
                            track.getHeight()));
            if (!center.has_value())
                continue;
            const auto centerY =
                track.getY()
                + static_cast<float>(
                    *center);
            graphics.setColour(
                panelBorderColour
                    .withAlpha(0.65F));
            graphics.drawHorizontalLine(
                static_cast<int>(centerY),
                timeTrack.getX() + 2.0F,
                timeTrack.getRight() - 2.0F);
            if (laneIndex > 0)
            {
                const auto separatorY =
                    track.getY()
                    + static_cast<float>(
                        static_cast<double>(
                            laneIndex)
                            * laneHeight
                        - scrollOffset);
                graphics.setColour(
                    panelBorderColour
                        .withAlpha(0.35F));
                graphics.drawHorizontalLine(
                    static_cast<int>(
                        separatorY),
                    track.getX() + 2.0F,
                    track.getRight() - 2.0F);
            }
            if (laneIndex < lanes.size())
            {
                const auto laneTop =
                    static_cast<double>(
                        laneIndex)
                        * laneHeight
                    - scrollOffset;
                const auto visibleTop =
                    std::max(
                        0.0,
                        laneTop);
                const auto visibleBottom =
                    std::min(
                        static_cast<double>(
                            track.getHeight()),
                        laneTop
                            + laneHeight);
                graphics.setColour(
                    secondaryTextColour);
                graphics.setFont(WorkbenchType::micro());
                graphics.drawText(
                    juceString(
                        lanes[laneIndex]
                            .eventName),
                    juce::Rectangle<float>{
                        track.getX()
                            + 4.0F,
                        track.getY()
                            + static_cast<float>(
                                visibleTop),
                        std::max(
                            0.0F,
                            timeTrack.getX()
                                - track.getX()
                                - 8.0F),
                        static_cast<float>(
                            std::max(
                                0.0,
                                visibleBottom
                                    - visibleTop))},
                    juce::Justification::
                        centredLeft);
            }
        }

        const auto& loop =
            bridge_.editor().document().timeline.loop;
        if (loop.enabled)
        {
            const auto width =
                std::max(
                    0.0F,
                    timeTrack.getWidth()
                        - 1.0F);
            const auto startX = timeTrack.getX()
                + static_cast<float>(
                    loop.startSeconds / duration)
                    * width;
            const auto endX = timeTrack.getX()
                + static_cast<float>(
                    loop.endSeconds / duration)
                    * width;
            graphics.setColour(controlColour.withAlpha(0.14F));
            graphics.fillRect(
                juce::Rectangle<float>{
                    startX,
                    track.getY() + 1.0F,
                    std::max(0.0F, endX - startX),
                    track.getHeight() - 2.0F});
            graphics.setColour(controlColour.withAlpha(0.9F));
            graphics.drawVerticalLine(
                static_cast<int>(startX),
                track.getY() + 2.0F,
                track.getBottom() - 2.0F);
            graphics.drawVerticalLine(
                static_cast<int>(endX),
                track.getY() + 2.0F,
                track.getBottom() - 2.0F);
            juce::Path startHandle;
            startHandle.addTriangle(
                startX,
                track.getY() + 2.0F,
                startX + 7.0F,
                track.getY() + 2.0F,
                startX,
                track.getY() + 9.0F);
            graphics.fillPath(startHandle);
            juce::Path endHandle;
            endHandle.addTriangle(
                endX,
                track.getY() + 2.0F,
                endX - 7.0F,
                track.getY() + 2.0F,
                endX,
                track.getY() + 9.0F);
            graphics.fillPath(endHandle);
        }
        const auto snapInterval =
            talas::project::timelineMarkerSnapInterval(
                duration,
                std::max(
                    1.0F,
                    timeTrack.getWidth()
                        - 1.0F));
        if (snapInterval > 0.0)
        {
            const auto tickCount = std::min<std::size_t>(
                512,
                static_cast<std::size_t>(
                    std::ceil(duration / snapInterval)));
            graphics.setColour(panelBorderColour.withAlpha(0.55F));
            for (std::size_t tick = 1; tick < tickCount; ++tick)
            {
                const auto time =
                    static_cast<double>(tick) * snapInterval;
                const auto x = timeTrack.getX()
                    + static_cast<float>(time / duration)
                        * std::max(
                            0.0F,
                            timeTrack.getWidth()
                                - 1.0F);
                graphics.drawVerticalLine(
                    static_cast<int>(x),
                    track.getY() + 2.0F,
                    track.getBottom()
                        - 2.0F);
            }
        }
        const auto eventPoint =
            [&](std::size_t index,
                std::size_t laneIndex) {
                const auto x =
                    timeTrack.getX()
                    + static_cast<float>(
                        events[index]
                            .timeSeconds
                        / duration)
                        * std::max(
                            0.0F,
                            timeTrack.getWidth()
                                - 1.0F);
                const auto& lane =
                    lanes[laneIndex];
                auto minimum =
                    events[lane.eventIndices.front()]
                        .value;
                auto maximum = minimum;
                for (const auto eventIndex :
                     lane.eventIndices)
                {
                    minimum = std::min(
                        minimum,
                        events[eventIndex]
                            .value);
                    maximum = std::max(
                        maximum,
                        events[eventIndex]
                            .value);
                }
                const auto normalized =
                    maximum > minimum
                    ? std::clamp(
                          (events[index].value
                           - minimum)
                              / (maximum
                                 - minimum),
                          0.0,
                          1.0)
                    : 0.5;
                const auto padding =
                    std::min(
                        3.0,
                        laneHeight * 0.2);
                const auto usableHeight =
                    std::max(
                        0.0,
                        laneHeight
                            - padding * 2.0);
                const auto y =
                    track.getY()
                    + static_cast<float>(
                        static_cast<double>(
                            laneIndex)
                            * laneHeight
                        - scrollOffset
                        + padding
                        + (1.0
                           - normalized)
                            * usableHeight);
                return juce::Point<float>{
                    x,
                    y,
                };
            };
        graphics.setColour(
            eventColour.withAlpha(0.58F));
        for (std::size_t laneIndex = 0;
             laneIndex < lanes.size();
             ++laneIndex)
        {
            const auto& lane =
                lanes[laneIndex];
            for (std::size_t position = 1;
                 position
                    < lane.eventIndices.size();
                 ++position)
            {
                const auto sourceIndex =
                    lane.eventIndices[
                        position - 1];
                const auto targetIndex =
                    lane.eventIndices[
                        position];
                const auto source =
                    eventPoint(
                        sourceIndex,
                        laneIndex);
                const auto target =
                    eventPoint(
                        targetIndex,
                        laneIndex);
                juce::Path segment;
                segment.startNewSubPath(
                    source);
                if (talas::project::
                        timelineSegmentUsesLinearInterpolation(
                            events[sourceIndex],
                            events[targetIndex],
                            loop,
                            playbackSampleRate_))
                {
                    segment.lineTo(target);
                }
                else
                {
                    segment.lineTo(
                        target.x,
                        source.y);
                    segment.lineTo(target);
                }
                graphics.strokePath(
                    segment,
                    juce::PathStrokeType{
                        1.35F});
            }
        }
        const auto selected = selectedRow();
        const auto drawMarker =
            [&](std::size_t index,
                std::size_t laneIndex,
                juce::Colour colour) {
            const auto laneCenter =
                talas::project::
                    timelineLaneVisibleCenterPosition(
                        laneIndex,
                        contentHeight,
                        laneCount,
                        scrollOffset,
                        static_cast<double>(
                            track.getHeight()));
            if (!laneCenter.has_value())
                return;
            const auto point =
                eventPoint(
                    index,
                    laneIndex);
            const auto x = point.x;
            const auto centerY = point.y;
            const auto markerDiameter =
                juce::jlimit(
                    3.0F,
                    7.0F,
                    static_cast<float>(
                        laneHeight)
                        * 0.55F);
            graphics.setColour(colour);
            graphics.drawVerticalLine(
                static_cast<int>(x),
                centerY
                    - std::max(
                        2.0F,
                        static_cast<float>(
                            laneHeight)
                            * 0.35F),
                centerY
                    + std::max(
                        2.0F,
                        static_cast<float>(
                            laneHeight)
                            * 0.35F));
            graphics.fillEllipse(
                x - markerDiameter * 0.5F,
                centerY
                    - markerDiameter * 0.5F,
                markerDiameter,
                markerDiameter);
        };
        for (std::size_t laneIndex = 0;
             laneIndex < lanes.size();
             ++laneIndex)
        {
            for (const auto index :
                 lanes[laneIndex]
                     .eventIndices)
            {
                if (static_cast<int>(
                        index)
                    != selected)
                {
                    drawMarker(
                        index,
                        laneIndex,
                        eventColour);
                }
            }
        }
        if (selected >= 0 && selected < static_cast<int>(events.size()))
        {
            const auto selectedIndex =
                static_cast<std::size_t>(
                    selected);
            const auto lane =
                std::ranges::find_if(
                    lanes,
                    [&](const auto& candidate) {
                        return std::ranges::find(
                                   candidate
                                       .eventIndices,
                                   selectedIndex)
                            != candidate
                                   .eventIndices.end();
                    });
            if (lane != lanes.end())
            {
                drawMarker(
                    selectedIndex,
                    static_cast<std::size_t>(
                        std::distance(
                            lanes.begin(),
                            lane)),
                    selectionColour);
            }
        }

        const auto playheadX = timeTrack.getX()
            + static_cast<float>(
                std::clamp(
                    transportTimeSeconds_,
                    0.0,
                    duration)
                / duration)
                * std::max(
                    0.0F,
                    timeTrack.getWidth()
                        - 1.0F);
        graphics.setColour(juce::Colour{talas::app::timelineTimeCursorColour()});
        graphics.drawVerticalLine(static_cast<int>(playheadX), track.getY() + 2.0F, track.getBottom() - 2.0F);
    }

    static juce::String formatTime(double timeSeconds)
    {
        const auto milliseconds = static_cast<std::int64_t>(std::llround(std::max(0.0, timeSeconds) * 1000.0));
        const auto minutes = milliseconds / 60000;
        const auto seconds = (milliseconds / 1000) % 60;
        const auto remainder = milliseconds % 1000;
        return juce::String(minutes).paddedLeft('0', 2)
            + ":" + juce::String(seconds).paddedLeft('0', 2)
            + "." + juce::String(remainder).paddedLeft('0', 3);
    }

    static juce::String formatEditableDouble(double value)
    {
        std::array<char, 64> buffer{};
        const auto [end, error] = std::to_chars(
            buffer.data(),
            buffer.data() + buffer.size(),
            value,
            std::chars_format::general,
            std::numeric_limits<double>::max_digits10);
        if (error != std::errc{})
            return {};
        return juce::String::fromUTF8(buffer.data(), static_cast<int>(end - buffer.data()));
    }

    static int domainId(talas::expr::ValueDomain domain)
    {
        switch (domain)
        {
        case talas::expr::ValueDomain::Scalar: return 1;
        case talas::expr::ValueDomain::Hertz: return 2;
        case talas::expr::ValueDomain::Seconds: return 3;
        case talas::expr::ValueDomain::NoteNumber: return 4;
        case talas::expr::ValueDomain::Samples: return 5;
        }
        return 1;
    }

    talas::expr::ValueDomain selectedDomain() const
    {
        switch (domainInput_.getSelectedId())
        {
        case 2: return talas::expr::ValueDomain::Hertz;
        case 3: return talas::expr::ValueDomain::Seconds;
        case 4: return talas::expr::ValueDomain::NoteNumber;
        case 5: return talas::expr::ValueDomain::Samples;
        default: return talas::expr::ValueDomain::Scalar;
        }
    }

    static int interpolationId(
        talas::core::AutomationInterpolation
            interpolation)
    {
        switch (interpolation)
        {
        case talas::core::AutomationInterpolation::Step:
            return 1;
        case talas::core::AutomationInterpolation::Linear:
            return 2;
        }
        return 1;
    }

    talas::core::AutomationInterpolation
    selectedInterpolation() const
    {
        return interpolationInput_
                       .getSelectedId()
                == 2
            ? talas::core::AutomationInterpolation::
                  Linear
            : talas::core::AutomationInterpolation::
                  Step;
    }

    static juce::String interpolationName(
        talas::core::AutomationInterpolation
            interpolation)
    {
        switch (interpolation)
        {
        case talas::core::AutomationInterpolation::Step:
            return "step";
        case talas::core::AutomationInterpolation::Linear:
            return "linear";
        }
        return {};
    }

    static juce::String domainName(talas::expr::ValueDomain domain)
    {
        switch (domain)
        {
        case talas::expr::ValueDomain::Scalar: return "scalar";
        case talas::expr::ValueDomain::Hertz: return "Hz";
        case talas::expr::ValueDomain::Seconds: return "s";
        case talas::expr::ValueDomain::NoteNumber: return "note";
        case talas::expr::ValueDomain::Samples: return "samples";
        }
        return {};
    }

    static constexpr int regularTimelineTrackHeight =
        70;
    static constexpr int compactTimelineTrackHeight =
        48;
    static constexpr int regularTimelinePanelHeight =
        294;
    static constexpr int compactTimelinePanelHeight =
        238;
    static constexpr int timelineLaneLabelWidth =
        72;
    static constexpr int timelineLaneScrollBarWidth =
        11;
    static constexpr double
        minimumTimelineLaneHeight = 16.0;

    talas::runtime::ProjectRuntimeBridge& bridge_;
    DocumentChangeCallback onDocumentChange_;
    StatusCallback onStatus_;
    TimelinePanel::OutputMeterSnapshotCallback outputMeters_;
    ScoreViewportComponent scoreViewport_;
    ControlsEditorComponent controlsEditor_;
    double controlsShare_ = 0.3;
    juce::ListBox list_;
    juce::ScrollBar laneScrollBar_{true};
    juce::ComboBox viewMode_;
    juce::ToggleButton loopButton_;
    juce::TextEditor loopStartInput_;
    juce::TextEditor loopEndInput_;
    juce::TextButton loopApplyButton_;
    juce::Label loopStartLabel_;
    juce::Label loopEndLabel_;
    juce::TextEditor timeInput_;
    juce::TextEditor nameInput_;
    juce::TextEditor valueInput_;
    juce::ComboBox domainInput_;
    juce::ComboBox interpolationInput_;
    juce::Label timeLabel_;
    juce::Label nameLabel_;
    juce::Label valueLabel_;
    juce::Label domainLabel_;
    juce::Label interpolationLabel_;
    juce::TextButton addButton_;
    juce::TextButton updateButton_;
    juce::TextButton removeButton_;
    juce::ComboBox musicalKindInput_;
    juce::TextEditor musicalTickInput_;
    juce::TextEditor musicalPrimaryInput_;
    juce::TextEditor musicalSecondaryInput_;
    juce::Label musicalKindLabel_;
    juce::Label musicalTickLabel_;
    juce::Label musicalPrimaryLabel_;
    juce::Label musicalSecondaryLabel_;
    std::vector<talas::project::TimelineLane>
        lanes_;
    std::vector<MusicalRow> musicalRows_;
    std::optional<talas::project::TimelineEventValue> selectedKey_;
    std::optional<talas::project::TimelineEventValue> pressedMarker_;
    std::optional<talas::project::TimelineLoopBoundary>
        pressedLoopBoundary_;
    int preferredRow_ = -1;
    ContentMode contentMode_ = ContentMode::Score;
    double transportTimeSeconds_ = 0.0;
    double playbackSampleRate_ = 48000.0;
    std::optional<talas::project::MusicalPosition>
        transportMusicalPosition_;
    juce::String transportSmptePosition_;
    juce::String externalTransportDetail_;
    talas::runtime::RealtimeMusicalTransportSnapshot latestTransportSnapshot_;
    double dragVisibleDurationSeconds_ = 1.0;
    double dragTrackWidthPixels_ = 1.0;
    std::optional<talas::project::ProjectEditor::HistoryTransactionToken>
        dragTransaction_;
    bool dragChanged_ = false;
};

}

std::unique_ptr<TimelinePanel> makeTimelinePanel(
    talas::runtime::ProjectRuntimeBridge& bridge,
    TimelinePanel::DocumentChangeCallback onDocumentChange,
    TimelinePanel::StatusCallback onStatus,
    TimelinePanel::OutputMeterSnapshotCallback outputMeters,
    TimelinePanel::AcceptedStageCurrentCallback acceptedStageCurrent,
    TimelinePanel::ClipArtifactStateCallback clipArtifactState,
    TimelinePanel::ClipArtifactActionCallback clipArtifactAction)
{
    return std::make_unique<TimelinePanelImpl>(
        bridge,
        std::move(onDocumentChange),
        std::move(onStatus),
        std::move(outputMeters),
        std::move(acceptedStageCurrent),
        std::move(clipArtifactState),
        std::move(clipArtifactAction));
}
}
