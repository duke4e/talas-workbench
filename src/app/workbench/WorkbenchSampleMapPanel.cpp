#include "app/workbench/WorkbenchSampleMapPanel.h"
#include "app/workbench/WorkbenchFocusCoordinator.h"

#include "app/workbench/WorkbenchTheme.h"
#include "app/workbench/WorkbenchUiHelpers.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <ranges>
#include <string_view>
#include <utility>

namespace talas::app::workbench
{
namespace
{
class SampleMapCanvasComponent final : public juce::Component
{
private:
    class AccessibilityRegion final : public juce::Component
    {
    public:
        AccessibilityRegion(
            SampleMapCanvasComponent& owner,
            std::string regionId,
            juce::String description,
            talas::app::SampleMapRect mapBounds,
            bool selected,
            bool preview)
            : owner_(owner)
            , regionId_(std::move(regionId))
            , selected_(selected)
            , mapBounds_(mapBounds)
            , preview_(preview)
        {
            setInterceptsMouseClicks(false, false);
            setWantsKeyboardFocus(true);
            describeControl(
                *this,
                description,
                preview
                    ? "Read-only import preview region. Adjust Low, Anchor, or High before applying the import."
                    : "Press to select this sample region. Use Up or Left for the previous region and Down or Right for the next region.");
        }

        [[nodiscard]] std::string_view regionId() const noexcept
        {
            return regionId_;
        }

        [[nodiscard]] const talas::app::SampleMapRect& mapBounds() const noexcept
        {
            return mapBounds_;
        }

        [[nodiscard]] bool isPreview() const noexcept
        {
            return preview_;
        }

        void paint(juce::Graphics& graphics) override
        {
            if (!hasKeyboardFocus(false))
                return;
            graphics.setColour(workbenchTheme.focus);
            graphics.drawRect(
                getLocalBounds().toFloat().reduced(1.0F),
                WorkbenchType::focusOutlinePixels());
        }

        bool keyPressed(const juce::KeyPress& key) override
        {
            if (preview_)
                return false;
            const auto code = key.getKeyCode();
            if (code == juce::KeyPress::returnKey
                || code == juce::KeyPress::spaceKey)
            {
                owner_.selectAccessibleRegion(regionId_, 0);
                return true;
            }
            if (code == juce::KeyPress::leftKey
                || code == juce::KeyPress::upKey)
            {
                owner_.selectAccessibleRegion(regionId_, -1);
                return true;
            }
            if (code == juce::KeyPress::rightKey
                || code == juce::KeyPress::downKey)
            {
                owner_.selectAccessibleRegion(regionId_, 1);
                return true;
            }
            return false;
        }

        void focusGained(juce::Component::FocusChangeType) override
        {
            repaint();
        }

        void focusLost(juce::Component::FocusChangeType) override
        {
            repaint();
        }

        std::unique_ptr<juce::AccessibilityHandler>
        createAccessibilityHandler() override
        {
            juce::Component::SafePointer<AccessibilityRegion> safeThis(this);
            auto actions = juce::AccessibilityActions{};
            actions.addAction(
                juce::AccessibilityActionType::focus,
                [safeThis] {
                    if (safeThis != nullptr)
                        safeThis->grabKeyboardFocus();
                });
            if (!preview_)
            {
                actions.addAction(
                    juce::AccessibilityActionType::press,
                    [safeThis] {
                        if (safeThis != nullptr)
                            safeThis->owner_.selectAccessibleRegion(
                                safeThis->regionId_,
                                0);
                    });
            }
            return std::make_unique<RegionAccessibilityHandler>(
                *this,
                std::move(actions));
        }

    private:
        class RegionAccessibilityHandler final
            : public juce::AccessibilityHandler
        {
        public:
            RegionAccessibilityHandler(
                AccessibilityRegion& region,
                juce::AccessibilityActions actions)
                : juce::AccessibilityHandler(
                      region,
                      juce::AccessibilityRole::treeItem,
                      std::move(actions))
                , region_(region)
            {
            }

            juce::AccessibleState getCurrentState() const override
            {
                auto state = juce::AccessibilityHandler::getCurrentState();
                if (region_.preview_)
                    return state;
                state = state.withSelectable();
                return region_.selected_
                    ? state.withSelected()
                    : state;
            }

        private:
            AccessibilityRegion& region_;
        };

        SampleMapCanvasComponent& owner_;
        std::string regionId_;
        bool selected_ = false;
        talas::app::SampleMapRect mapBounds_;
        bool preview_ = false;
    };

public:
    explicit SampleMapCanvasComponent(
        SampleMapPanelCallbacks& callbacks)
        : callbacks_(callbacks)
    {
        setWantsKeyboardFocus(true);
        describeControl(
            *this,
            "Sample key and velocity map",
            "Regions span MIDI notes horizontally and velocities vertically. "
            "Arrow keys move the selection; Shift plus arrows resize its high edge.");
    }

    void setState(const SampleMapPanelState& state)
    {
        juce::String focusedRegionId;
        for (auto* item : accessibilityRegions_)
        {
            if (item->hasKeyboardFocus(false))
            {
                focusedRegionId = juceString(item->regionId());
                break;
            }
        }
        state_ = state;
        const auto primary = std::ranges::find_if(
            state_.projection.regions,
            [](const auto& region) { return region.primary; });
        auto description = juce::String(static_cast<int>(
            state_.projection.regions.size()))
            + (state_.projection.regions.size() == 1
                   ? " sample region. "
                   : " sample regions. ");
        description += primary == state_.projection.regions.end()
            ? juce::String{"No primary sample region selected."}
            : "Selected: "
                + juceString(primary->accessibleDescription);
        if (!state_.importPreviewRegions.empty())
        {
            description += " Import preview: "
                + juce::String(static_cast<int>(
                    state_.importPreviewRegions.size()))
                + (state_.importPreviewRegions.size() == 1
                       ? " region."
                       : " regions.");
        }
        setDescription(description);
        rebuildAccessibilityRegions(focusedRegionId);
        repaint();
    }

    std::unique_ptr<juce::AccessibilityHandler>
    createAccessibilityHandler() override
    {
        return std::make_unique<juce::AccessibilityHandler>(
            *this,
            juce::AccessibilityRole::tree);
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        const auto code = key.getKeyCode();
        const auto horizontal = code == juce::KeyPress::leftKey
            || code == juce::KeyPress::rightKey;
        const auto vertical = code == juce::KeyPress::upKey
            || code == juce::KeyPress::downKey;
        if (code == juce::KeyPress::returnKey
            || code == juce::KeyPress::spaceKey)
        {
            return selectFirstAccessibleRegion();
        }
        if (!horizontal && !vertical)
            return false;
        if (!hasPrimaryRegion())
            return selectFirstAccessibleRegion();
        const auto delta = code == juce::KeyPress::leftKey
                || code == juce::KeyPress::downKey
            ? -1
            : 1;
        if (key.getModifiers().isShiftDown())
        {
            if (callbacks_.resizePrimaryRegion)
            {
                callbacks_.resizePrimaryRegion(
                    horizontal
                        ? talas::app::SampleMapResizeEdge::HighNote
                        : talas::app::SampleMapResizeEdge::HighVelocity,
                    delta);
            }
        }
        else if (callbacks_.moveSelection)
        {
            callbacks_.moveSelection(
                horizontal ? delta : 0,
                vertical ? delta : 0);
        }
        return true;
    }

    void resized() override
    {
        layoutAccessibilityRegions();
    }

    [[nodiscard]] std::size_t accessibleRegionCount() const noexcept
    {
        return static_cast<std::size_t>(accessibilityRegions_.size());
    }

    [[nodiscard]] bool hasAccessibleImportPreviewForSmoke() const
    {
        return hasAccessibleImportPreview();
    }

    [[nodiscard]] bool selectFirstAccessibleRegion()
    {
        if (state_.projection.regions.empty())
            return false;
        selectAccessibleRegion(state_.projection.regions.front().regionId, 0);
        return true;
    }

    [[nodiscard]] bool selectFirstOrNextAccessibleRegion()
    {
        const auto primary = std::ranges::find_if(
            state_.projection.regions,
            [](const auto& region) { return region.primary; });
        if (primary == state_.projection.regions.end())
            return selectFirstAccessibleRegion();
        selectAccessibleRegion(primary->regionId, 1);
        return true;
    }

    void mouseDown(const juce::MouseEvent& event) override
    {
        grabKeyboardFocus();
        dragStartMidi_ = midiAt(event.position);
        dragRegionId_.reset();
        dragResizeEdge_.reset();
        for (auto iterator = state_.projection.regions.rbegin();
             iterator != state_.projection.regions.rend();
             ++iterator)
        {
            if (!scaledBounds(iterator->bounds).contains(event.position))
                continue;
            dragRegionId_ = iterator->regionId;
            const auto hit = talas::app::hitTestSampleMapRegion(
                {
                    scaledBounds(iterator->bounds).getX(),
                    scaledBounds(iterator->bounds).getY(),
                    scaledBounds(iterator->bounds).getWidth(),
                    scaledBounds(iterator->bounds).getHeight(),
                },
                event.position.x,
                event.position.y,
                5.0);
            if (hit.lowNoteEdge)
                dragResizeEdge_ = talas::app::SampleMapResizeEdge::LowNote;
            else if (hit.highNoteEdge)
                dragResizeEdge_ = talas::app::SampleMapResizeEdge::HighNote;
            else if (hit.lowVelocityEdge)
                dragResizeEdge_ = talas::app::SampleMapResizeEdge::LowVelocity;
            else if (hit.highVelocityEdge)
                dragResizeEdge_ = talas::app::SampleMapResizeEdge::HighVelocity;
            if (callbacks_.selectRegion)
            {
                const auto modifiers = event.mods;
                const auto mode = modifiers.isCommandDown()
                    ? talas::app::SampleMapSelectionMode::Toggle
                    : (modifiers.isShiftDown()
                           ? talas::app::SampleMapSelectionMode::Add
                           : talas::app::SampleMapSelectionMode::Replace);
                callbacks_.selectRegion(iterator->regionId, mode);
            }
            return;
        }
        if (const auto midi = midiAt(event.position);
            midi.has_value() && callbacks_.selectByMidi)
        {
            callbacks_.selectByMidi(midi->note, midi->velocity);
        }
    }

    void mouseUp(const juce::MouseEvent& event) override
    {
        if (!dragRegionId_.has_value() || !dragStartMidi_.has_value())
            return;
        const auto current = midiAt(event.position);
        if (!current.has_value())
            return;
        const auto noteDelta = static_cast<int>(current->note)
            - static_cast<int>(dragStartMidi_->note);
        const auto velocityDelta = static_cast<int>(current->velocity)
            - static_cast<int>(dragStartMidi_->velocity);
        if (dragResizeEdge_.has_value())
        {
            const auto horizontal = *dragResizeEdge_
                    == talas::app::SampleMapResizeEdge::LowNote
                || *dragResizeEdge_
                    == talas::app::SampleMapResizeEdge::HighNote;
            const auto delta = horizontal ? noteDelta : velocityDelta;
            if (delta != 0 && callbacks_.resizePrimaryRegion)
                callbacks_.resizePrimaryRegion(*dragResizeEdge_, delta);
            return;
        }
        if ((noteDelta != 0 || velocityDelta != 0)
            && callbacks_.moveSelection)
        {
            callbacks_.moveSelection(noteDelta, velocityDelta);
        }
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(backgroundColour);
        const auto bounds = getLocalBounds().toFloat();
        graphics.setColour(panelBorderColour);
        graphics.drawRect(getLocalBounds(), 1);

        if (state_.projection.activeMapId.has_value())
        {
            graphics.setColour(workbenchTheme.graphGrid);
            for (int note = 0; note <= 128; note += 12)
            {
                const auto x = static_cast<float>(
                    talas::app::sampleMapNoteBoundaryToPixel(
                        static_cast<double>(note),
                        state_.viewport,
                        {bounds.getWidth(), bounds.getHeight()}));
                graphics.drawVerticalLine(
                    static_cast<int>(std::lround(x)),
                    0.0F,
                    bounds.getHeight());
            }
            for (int velocity : {1, 32, 64, 96, 128})
            {
                const auto y = static_cast<float>(
                    talas::app::sampleMapVelocityBoundaryToPixel(
                        static_cast<double>(velocity),
                        state_.viewport,
                        {bounds.getWidth(), bounds.getHeight()}));
                graphics.drawHorizontalLine(
                    static_cast<int>(std::lround(y)),
                    0.0F,
                    bounds.getWidth());
            }
        }

        for (const auto& region : state_.projection.regions)
        {
            auto rect = scaledBounds(region.bounds).reduced(1.0F);
            const auto fill = region.selected
                ? workbenchTheme.selectedRow
                : workbenchTheme.data.withAlpha(0.28F);
            graphics.setColour(fill);
            graphics.fillRect(rect);
            graphics.setColour(
                region.primary ? workbenchTheme.focus : workbenchTheme.data);
            graphics.drawRect(rect, region.primary ? 2.0F : 1.0F);
            if (rect.getWidth() >= 52.0F && rect.getHeight() >= 18.0F)
            {
                graphics.setColour(textColour);
                graphics.setFont(WorkbenchType::microStrong());
                graphics.drawFittedText(
                    juceString(region.assetId),
                    rect.toNearestInt().reduced(4, 2),
                    juce::Justification::centredLeft,
                    1);
            }
        }

        for (const auto& preview : state_.importPreviewRegions)
        {
            auto rect = scaledBounds(preview.bounds).reduced(2.0F);
            graphics.setColour(workbenchTheme.data.withAlpha(0.18F));
            graphics.fillRect(rect);
            graphics.setColour(workbenchTheme.data.withAlpha(0.92F));
            const float dashLengths[] = {5.0F, 3.0F};
            graphics.drawDashedLine(
                juce::Line<float>{rect.getTopLeft(), rect.getTopRight()},
                dashLengths,
                2,
                1.5F);
            graphics.drawDashedLine(
                juce::Line<float>{rect.getBottomLeft(), rect.getBottomRight()},
                dashLengths,
                2,
                1.5F);
            graphics.drawDashedLine(
                juce::Line<float>{rect.getTopLeft(), rect.getBottomLeft()},
                dashLengths,
                2,
                1.5F);
            graphics.drawDashedLine(
                juce::Line<float>{rect.getTopRight(), rect.getBottomRight()},
                dashLengths,
                2,
                1.5F);
            if (rect.getWidth() >= 52.0F && rect.getHeight() >= 18.0F)
            {
                graphics.setColour(textColour);
                graphics.setFont(WorkbenchType::microStrong());
                graphics.drawFittedText(
                    juceString(preview.region.assetId),
                    rect.toNearestInt().reduced(4, 2),
                    juce::Justification::centredLeft,
                    1);
            }
        }

        if (state_.midiPosition.has_value())
        {
            const auto x = static_cast<float>(
                talas::app::sampleMapNoteBoundaryToPixel(
                    static_cast<double>(state_.midiPosition->note) + 0.5,
                    state_.viewport,
                    {bounds.getWidth(), bounds.getHeight()}));
            const auto y = static_cast<float>(
                talas::app::sampleMapVelocityBoundaryToPixel(
                    static_cast<double>(state_.midiPosition->velocity),
                    state_.viewport,
                    {bounds.getWidth(), bounds.getHeight()}));
            graphics.setColour(workbenchTheme.event);
            graphics.drawVerticalLine(
                static_cast<int>(std::lround(x)),
                0.0F,
                bounds.getHeight());
            graphics.fillEllipse(x - 3.0F, y - 3.0F, 6.0F, 6.0F);
        }

        if (!state_.projection.activeMapId.has_value())
            drawEmpty(graphics, "Create a sample map to begin");
        else if (state_.projection.regions.empty()
                 && state_.importPreviewRegions.empty())
            drawEmpty(graphics, "Import PCM16 WAV files or add regions");
    }

private:
    [[nodiscard]] bool hasPrimaryRegion() const
    {
        return std::ranges::any_of(
            state_.projection.regions,
            [](const auto& region) { return region.primary; });
    }

    void selectAccessibleRegion(std::string_view regionId, int offset)
    {
        if (state_.projection.regions.empty())
            return;
        auto found = std::ranges::find(
            state_.projection.regions,
            regionId,
            &talas::app::SampleMapRegionProjection::regionId);
        if (found == state_.projection.regions.end())
            found = state_.projection.regions.begin();
        auto index = static_cast<std::ptrdiff_t>(
            std::distance(state_.projection.regions.begin(), found));
        index = std::clamp<std::ptrdiff_t>(
            index + offset,
            0,
            static_cast<std::ptrdiff_t>(state_.projection.regions.size() - 1));
        const auto targetId = state_.projection.regions[
            static_cast<std::size_t>(index)].regionId;
        if (callbacks_.selectRegion)
        {
            callbacks_.selectRegion(
                targetId,
                talas::app::SampleMapSelectionMode::Replace);
        }
        for (auto* item : accessibilityRegions_)
        {
            if (item->regionId() == targetId)
            {
                if (item->isShowing())
                    item->grabKeyboardFocus();
                break;
            }
        }
    }

    void rebuildAccessibilityRegions(const juce::String& focusedRegionId)
    {
        accessibilityRegions_.clear(true);
        int focusOrder = 1;
        for (const auto& region : state_.projection.regions)
        {
            auto* item = accessibilityRegions_.add(
                new AccessibilityRegion(
                    *this,
                    region.regionId,
                    juceString(region.accessibleDescription),
                    region.bounds,
                    region.selected,
                    false));
            const std::array regionFocusFields{
                std::string_view{region.regionId}};
            item->setComponentID(juceString(
                workbenchStableFocusItemId(
                    "sample-map.region", regionFocusFields)));
            item->setExplicitFocusOrder(focusOrder++);
            addAndMakeVisible(item);
        }
        for (const auto& preview : state_.importPreviewRegions)
        {
            const auto& region = preview.region;
            auto description = "Import preview " + juceString(region.assetId)
                + ": notes " + juce::String(region.lowNote)
                + " through " + juce::String(region.highNote)
                + ", root " + juce::String(region.rootNote)
                + ", velocities " + juce::String(region.lowVelocity)
                + " through " + juce::String(region.highVelocity);
            auto* item = accessibilityRegions_.add(
                new AccessibilityRegion(
                    *this,
                    "import-preview:" + region.regionId,
                    std::move(description),
                    preview.bounds,
                    false,
                    true));
            const std::array previewFocusFields{
                std::string_view{region.regionId}};
            item->setComponentID(juceString(
                workbenchStableFocusItemId(
                    "sample-map.import-preview", previewFocusFields)));
            item->setExplicitFocusOrder(focusOrder++);
            addAndMakeVisible(item);
        }
        layoutAccessibilityRegions();
        if (focusedRegionId.isNotEmpty())
        {
            for (auto* item : accessibilityRegions_)
            {
                if (juceString(item->regionId()) == focusedRegionId)
                {
                    item->grabKeyboardFocus();
                    break;
                }
            }
        }
        if (auto* handler = getAccessibilityHandler())
        {
            handler->notifyAccessibilityEvent(
                juce::AccessibilityEvent::structureChanged);
        }
    }

    void layoutAccessibilityRegions()
    {
        for (auto* region : accessibilityRegions_)
        {
            region->setBounds(
                scaledBounds(region->mapBounds()).toNearestInt());
        }
    }

    [[nodiscard]] bool hasAccessibleImportPreview() const
    {
        std::size_t previewIndex = 0;
        for (const auto* item : accessibilityRegions_)
        {
            if (!item->isPreview())
                continue;
            if (previewIndex >= state_.importPreviewRegions.size())
                return false;
            const auto& region =
                state_.importPreviewRegions[previewIndex++].region;
            const auto title = item->getTitle();
            if (!title.contains(juceString(region.assetId))
                || !title.contains("notes " + juce::String(region.lowNote)
                                   + " through "
                                   + juce::String(region.highNote))
                || !title.contains("root " + juce::String(region.rootNote))
                || !title.contains(
                    "velocities " + juce::String(region.lowVelocity)
                    + " through " + juce::String(region.highVelocity)))
            {
                return false;
            }
        }
        return previewIndex == state_.importPreviewRegions.size()
            && previewIndex > 0;
    }

    void drawEmpty(juce::Graphics& graphics, const juce::String& text)
    {
        graphics.setColour(secondaryTextColour);
        graphics.setFont(WorkbenchType::bodyStrong());
        graphics.drawFittedText(
            text,
            getLocalBounds().reduced(24),
            juce::Justification::centred,
            2);
    }

    [[nodiscard]] juce::Rectangle<float> scaledBounds(
        const talas::app::SampleMapRect& bounds) const
    {
        const auto width = std::max(1.0, state_.projectionCanvas.width);
        const auto height = std::max(1.0, state_.projectionCanvas.height);
        return {
            static_cast<float>(bounds.x / width * getWidth()),
            static_cast<float>(bounds.y / height * getHeight()),
            static_cast<float>(bounds.width / width * getWidth()),
            static_cast<float>(bounds.height / height * getHeight()),
        };
    }

    [[nodiscard]] std::optional<talas::app::SampleMapMidiPoint> midiAt(
        juce::Point<float> point) const
    {
        return talas::app::sampleMapMidiAtPoint(
            point.x,
            point.y,
            state_.viewport,
            {static_cast<double>(getWidth()),
             static_cast<double>(getHeight())});
    }

    SampleMapPanelCallbacks& callbacks_;
    SampleMapPanelState state_;
    juce::OwnedArray<AccessibilityRegion> accessibilityRegions_;
    std::optional<talas::app::SampleMapMidiPoint> dragStartMidi_;
    std::optional<std::string> dragRegionId_;
    std::optional<talas::app::SampleMapResizeEdge> dragResizeEdge_;
};

class SampleMapPanelImpl final : public SampleMapPanel,
                                 private juce::ListBoxModel
{
public:
    explicit SampleMapPanelImpl(SampleMapPanelCallbacks callbacks)
        : callbacks_(std::move(callbacks))
        , createButton_("New Map")
        , renameButton_("Rename")
        , removeButton_("Remove")
        , importButton_("Import WAV...")
        , replaceButton_("Replace WAV...")
        , cancelButton_("Cancel")
        , applyButton_("Apply Import")
        , canvas_(callbacks_)
    {
        describeControl(
            *this,
            "Samples workspace",
            "Build deterministic MIDI note and velocity regions from project audio assets.");
        title_.setText("Sample Maps", juce::dontSendNotification);
        title_.setFont(WorkbenchType::panelTitle());
        title_.setColour(juce::Label::textColourId, textColour);
        status_.setFont(WorkbenchType::micro());
        status_.setColour(juce::Label::textColourId, secondaryTextColour);
        status_.setMinimumHorizontalScale(1.0F);
        progress_.setPercentageDisplay(true);
        progress_.setVisible(false);

        list_.setModel(this);
        list_.setRowHeight(36);
        list_.setWantsKeyboardFocus(true);
        list_.setColour(juce::ListBox::backgroundColourId, panelColour);
        list_.setColour(juce::ListBox::outlineColourId, panelBorderColour);
        describeControl(
            list_,
            "Project sample maps",
            "Choose the active portable sample map with the arrow keys.");

        mapName_.setTextToShowWhenEmpty("Map name", secondaryTextColour);
        mapName_.setSelectAllWhenFocused(true);
        describeControl(
            mapName_,
            "Sample map name",
            "Enter a portable display name, then choose Rename.");

        disposition_.addItem("Reference", 1);
        disposition_.addItem("Embed", 2);
        disposition_.setSelectedId(1, juce::dontSendNotification);
        describeControl(
            disposition_,
            "Import disposition",
            "Reference keeps source paths; Embed copies verified bytes beside a saved project.");
        replaceConfirm_.setTextToShowWhenEmpty(
            "REPLACE",
            secondaryTextColour);
        replaceConfirm_.setInputRestrictions(7, "REPLACE");
        replaceConfirm_.setSelectAllWhenFocused(true);
        describeControl(
            replaceConfirm_,
            "Replace confirmation",
            "Type REPLACE exactly to enable destructive replacement of every "
            "region in the active map. The project editor also verifies the map "
            "revision before publication.");

        lowLabel_.setText("Low", juce::dontSendNotification);
        anchorLabel_.setText("Anchor", juce::dontSendNotification);
        highLabel_.setText("High", juce::dontSendNotification);
        for (auto* label : {&lowLabel_, &anchorLabel_, &highLabel_})
        {
            label->setFont(WorkbenchType::microStrong());
            label->setColour(juce::Label::textColourId, secondaryTextColour);
            label->setJustificationType(juce::Justification::centredLeft);
        }
        for (int note = 0; note <= 127; ++note)
        {
            lowNote_.addItem(juce::String(note), note + 1);
            anchorNote_.addItem(juce::String(note), note + 1);
            highNote_.addItem(juce::String(note), note + 1);
        }
        lowNote_.setWantsKeyboardFocus(true);
        anchorNote_.setWantsKeyboardFocus(true);
        highNote_.setWantsKeyboardFocus(true);
        applyButton_.setWantsKeyboardFocus(true);
        describeControl(
            lowNote_,
            "Import low MIDI note",
            "Choose the first note in the deterministic auto-map range.");
        describeControl(
            anchorNote_,
            "Import anchor root note",
            "Choose the visible root-note anchor for the first imported source.");
        describeControl(
            highNote_,
            "Import high MIDI note",
            "Choose the final note in the deterministic auto-map range.");
        describeControl(
            applyButton_,
            "Apply sample import",
            "Publish the visible preview as one undoable project edit.");

        const std::array<juce::Component*, 21> components = {
            &title_, &status_, &list_, &mapName_, &createButton_,
            &renameButton_, &removeButton_, &disposition_,
            &importButton_, &replaceConfirm_, &replaceButton_,
            &cancelButton_, &applyButton_, &lowLabel_, &anchorLabel_,
            &highLabel_, &lowNote_, &anchorNote_, &highNote_,
            &canvas_, &progress_,
        };
        for (auto* component : components)
        {
            addAndMakeVisible(*component);
        }
        createButton_.onClick = [this] {
            if (callbacks_.createMap)
                callbacks_.createMap();
        };
        renameButton_.onClick = [this] {
            if (callbacks_.renameMap && mapName_.getText().isNotEmpty())
                callbacks_.renameMap(mapName_.getText().toStdString());
        };
        removeButton_.onClick = [this] {
            if (callbacks_.removeMap && state_.projection.activeMapId.has_value())
                callbacks_.removeMap(*state_.projection.activeMapId);
        };
        importButton_.onClick = [this] {
            if (callbacks_.importFiles)
                callbacks_.importFiles();
        };
        replaceButton_.onClick = [this] {
            if (replaceConfirm_.getText() != "REPLACE")
            {
                status_.setText(
                    "Type REPLACE exactly before replacing all regions",
                    juce::dontSendNotification);
                status_.setTitle(status_.getText());
                return;
            }
            replaceConfirm_.clear();
            if (callbacks_.replaceFiles)
                callbacks_.replaceFiles();
        };
        cancelButton_.onClick = [this] {
            if (callbacks_.cancelImport)
                callbacks_.cancelImport();
        };
        applyButton_.onClick = [this] {
            if (callbacks_.applyImport)
                callbacks_.applyImport();
        };
        const auto rangeChanged = [this] {
            if (refreshing_ || !callbacks_.setImportRange)
                return;
            callbacks_.setImportRange(
                static_cast<std::uint8_t>(lowNote_.getSelectedId() - 1),
                static_cast<std::uint8_t>(anchorNote_.getSelectedId() - 1),
                static_cast<std::uint8_t>(highNote_.getSelectedId() - 1));
        };
        lowNote_.onChange = rangeChanged;
        anchorNote_.onChange = rangeChanged;
        highNote_.onChange = rangeChanged;
        disposition_.onChange = [this] {
            if (!refreshing_ && callbacks_.setEmbedImports)
                callbacks_.setEmbedImports(disposition_.getSelectedId() == 2);
        };
    }

    ~SampleMapPanelImpl() override
    {
        list_.setModel(nullptr);
    }

    std::unique_ptr<juce::AccessibilityHandler>
    createAccessibilityHandler() override
    {
        return std::make_unique<juce::AccessibilityHandler>(
            *this,
            juce::AccessibilityRole::group);
    }

    void setState(const SampleMapPanelState& state) override
    {
        refreshing_ = true;
        state_ = state;
        canvas_.setState(state_);
        status_.setText(
            juceString(state_.statusText),
            juce::dontSendNotification);
        status_.setTitle(juceString(state_.statusText));
        disposition_.setSelectedId(
            state_.embedImports ? 2 : 1,
            juce::dontSendNotification);
        lowNote_.setSelectedId(
            static_cast<int>(state_.importLowNote) + 1,
            juce::dontSendNotification);
        anchorNote_.setSelectedId(
            static_cast<int>(state_.importAnchorRootNote) + 1,
            juce::dontSendNotification);
        highNote_.setSelectedId(
            static_cast<int>(state_.importHighNote) + 1,
            juce::dontSendNotification);
        list_.updateContent();
        auto activeRow = -1;
        for (std::size_t index = 0; index < state_.projection.maps.size(); ++index)
        {
            if (state_.projection.maps[index].active)
                activeRow = static_cast<int>(index);
        }
        list_.selectRow(activeRow, false, true);
        if (activeRow >= 0)
        {
            mapName_.setText(
                juceString(state_.projection.maps[static_cast<std::size_t>(activeRow)].displayName),
                false);
        }
        else
        {
            mapName_.clear();
        }
        const auto importing = state_.importState
            == SampleMapImportPresentation::Running;
        const auto previewing = state_.importState
            == SampleMapImportPresentation::Preview;
        progressValue_ = std::clamp(state_.importProgress, 0.0, 1.0);
        progress_.setVisible(importing);
        cancelButton_.setVisible(importing || previewing);
        applyButton_.setVisible(previewing);
        applyButton_.setEnabled(previewing && state_.importPreviewValid);
        for (auto* component : {
                 static_cast<juce::Component*>(&lowLabel_),
                 static_cast<juce::Component*>(&anchorLabel_),
                 static_cast<juce::Component*>(&highLabel_),
                 static_cast<juce::Component*>(&lowNote_),
                 static_cast<juce::Component*>(&anchorNote_),
                 static_cast<juce::Component*>(&highNote_)})
        {
            component->setVisible(previewing);
        }
        const auto locked = importing || previewing;
        createButton_.setEnabled(!locked);
        renameButton_.setEnabled(!locked && activeRow >= 0);
        removeButton_.setEnabled(!locked && activeRow >= 0);
        importButton_.setEnabled(!locked && activeRow >= 0);
        importButton_.setVisible(!previewing);
        replaceButton_.setEnabled(!locked && activeRow >= 0);
        replaceButton_.setVisible(!locked);
        replaceConfirm_.setEnabled(!locked && activeRow >= 0);
        replaceConfirm_.setVisible(!locked);
        disposition_.setEnabled(!locked);
        disposition_.setVisible(!previewing);
        refreshing_ = false;
        resized();
        repaint();
    }

    [[nodiscard]] bool hasReadySurfaceForSmoke() const override
    {
        return !getBounds().isEmpty()
            && !canvas_.getBounds().isEmpty()
            && !list_.getBounds().isEmpty()
            && state_.projection.activeMapId.has_value();
    }

    [[nodiscard]] bool hasAccessibleEditorsForSmoke() const override
    {
        return canvas_.getWantsKeyboardFocus()
            && list_.getWantsKeyboardFocus()
            && mapName_.getWantsKeyboardFocus()
            && importButton_.getWantsKeyboardFocus();
    }

    [[nodiscard]] std::size_t accessibleRegionCountForSmoke() const override
    {
        return canvas_.accessibleRegionCount();
    }

    [[nodiscard]] bool invokeKeyboardRegionSelectionForSmoke() override
    {
        return canvas_.selectFirstOrNextAccessibleRegion();
    }

    [[nodiscard]] bool invokeKeyboardMoveForSmoke() override
    {
        return canvas_.keyPressed(
            juce::KeyPress(juce::KeyPress::rightKey));
    }

    [[nodiscard]] bool hasImportPreviewControlsForSmoke() const override
    {
        return state_.importState == SampleMapImportPresentation::Preview
            && lowNote_.isVisible()
            && anchorNote_.isVisible()
            && highNote_.isVisible()
            && applyButton_.isVisible()
            && cancelButton_.isVisible()
            && lowNote_.getTitle().isNotEmpty()
            && anchorNote_.getTitle().isNotEmpty()
            && highNote_.getTitle().isNotEmpty()
            && applyButton_.getTitle().isNotEmpty();
    }

    [[nodiscard]] bool hasAccessibleImportPreviewForSmoke() const override
    {
        return canvas_.hasAccessibleImportPreviewForSmoke();
    }

    [[nodiscard]] bool setImportPreviewRangeForSmoke(
        std::uint8_t lowNote,
        std::uint8_t anchorRootNote,
        std::uint8_t highNote) override
    {
        if (!hasImportPreviewControlsForSmoke()
            || !callbacks_.setImportRange)
        {
            return false;
        }
        callbacks_.setImportRange(lowNote, anchorRootNote, highNote);
        return true;
    }

    [[nodiscard]] bool applyImportPreviewForSmoke() override
    {
        if (!hasImportPreviewControlsForSmoke()
            || !state_.importPreviewValid
            || !callbacks_.applyImport)
        {
            return false;
        }
        callbacks_.applyImport();
        return true;
    }

    [[nodiscard]] bool cancelImportPreviewForSmoke() override
    {
        if (!hasImportPreviewControlsForSmoke()
            || !callbacks_.cancelImport)
        {
            return false;
        }
        callbacks_.cancelImport();
        return true;
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(panelColour);
        graphics.setColour(panelBorderColour);
        graphics.drawRect(getLocalBounds(), 1);
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(8);
        auto left = bounds.removeFromLeft(
            getWidth() < 760 ? 184 : 224);
        bounds.removeFromLeft(8);
        title_.setBounds(left.removeFromTop(20));
        status_.setBounds(left.removeFromTop(30));
        left.removeFromTop(4);
        const auto previewing = state_.importState
            == SampleMapImportPresentation::Preview;
        auto importArea = left.removeFromBottom(
            previewing ? 92 : (progress_.isVisible() ? 94 : 60));
        left.removeFromBottom(6);
        auto nameRow = left.removeFromBottom(28);
        mapName_.setBounds(nameRow.removeFromLeft(
            std::max(72, nameRow.getWidth() - 70)));
        nameRow.removeFromLeft(6);
        renameButton_.setBounds(nameRow);
        left.removeFromBottom(6);
        auto mapActions = left.removeFromBottom(28);
        const auto actionWidth = (mapActions.getWidth() - 6) / 2;
        createButton_.setBounds(mapActions.removeFromLeft(actionWidth));
        mapActions.removeFromLeft(6);
        removeButton_.setBounds(mapActions);
        left.removeFromBottom(6);
        list_.setBounds(left);

        if (previewing)
        {
            auto labels = importArea.removeFromTop(16);
            auto editors = importArea.removeFromTop(28);
            constexpr int gap = 4;
            const auto width = (editors.getWidth() - gap * 2) / 3;
            lowLabel_.setBounds(labels.removeFromLeft(width));
            labels.removeFromLeft(gap);
            anchorLabel_.setBounds(labels.removeFromLeft(width));
            labels.removeFromLeft(gap);
            highLabel_.setBounds(labels);
            lowNote_.setBounds(editors.removeFromLeft(width));
            editors.removeFromLeft(gap);
            anchorNote_.setBounds(editors.removeFromLeft(width));
            editors.removeFromLeft(gap);
            highNote_.setBounds(editors);
            importArea.removeFromTop(6);
            auto actions = importArea.removeFromTop(28);
            const auto cancelWidth = 72;
            applyButton_.setBounds(actions.removeFromLeft(
                std::max(0, actions.getWidth() - cancelWidth - 6)));
            actions.removeFromLeft(6);
            cancelButton_.setBounds(actions);
            disposition_.setBounds({});
            importButton_.setBounds({});
            replaceConfirm_.setBounds({});
            replaceButton_.setBounds({});
            progress_.setBounds({});
        }
        else
        {
            auto importRow = importArea.removeFromTop(28);
            disposition_.setBounds(importRow.removeFromLeft(92));
            importRow.removeFromLeft(6);
            importButton_.setBounds(importRow.removeFromLeft(
                std::max(0, importRow.getWidth() - (cancelButton_.isVisible() ? 72 : 0))));
            if (cancelButton_.isVisible())
            {
                importRow.removeFromLeft(6);
                cancelButton_.setBounds(importRow);
            }
            else
            {
                cancelButton_.setBounds({});
            }
            auto replaceRow = importArea.removeFromTop(28);
            if (replaceButton_.isVisible())
            {
                replaceConfirm_.setBounds(replaceRow.removeFromLeft(
                    std::max(76, replaceRow.getWidth() - 112)));
                replaceRow.removeFromLeft(6);
                replaceButton_.setBounds(replaceRow);
            }
            else
            {
                replaceConfirm_.setBounds({});
                replaceButton_.setBounds({});
            }
            if (progress_.isVisible())
            {
                importArea.removeFromTop(6);
                progress_.setBounds(importArea.removeFromTop(18));
            }
            else
            {
                progress_.setBounds({});
            }
            applyButton_.setBounds({});
            lowLabel_.setBounds({});
            anchorLabel_.setBounds({});
            highLabel_.setBounds({});
            lowNote_.setBounds({});
            anchorNote_.setBounds({});
            highNote_.setBounds({});
        }
        canvas_.setBounds(bounds);
    }

private:
    void selectedRowsChanged(int lastRowSelected) override
    {
        if (refreshing_ || lastRowSelected < 0
            || lastRowSelected
                >= static_cast<int>(state_.projection.maps.size()))
        {
            return;
        }
        const auto& map = state_.projection.maps[
            static_cast<std::size_t>(lastRowSelected)];
        mapName_.setText(juceString(map.displayName), false);
        if (callbacks_.selectMap)
            callbacks_.selectMap(map.mapId);
    }

    int getNumRows() override
    {
        return static_cast<int>(state_.projection.maps.size());
    }

    juce::String getNameForRow(int row) override
    {
        if (row < 0 || row >= static_cast<int>(state_.projection.maps.size()))
            return {};
        const auto& map = state_.projection.maps[static_cast<std::size_t>(row)];
        return juceString(map.displayName)
            + juceString(" · ")
            + juce::String(static_cast<int>(map.regionCount))
            + (map.regionCount == 1 ? " region" : " regions");
    }

    void paintListBoxItem(
        int row,
        juce::Graphics& graphics,
        int width,
        int height,
        bool selected) override
    {
        if (row < 0 || row >= static_cast<int>(state_.projection.maps.size()))
            return;
        if (selected)
            graphics.fillAll(workbenchTheme.selectedRow);
        const auto& map = state_.projection.maps[static_cast<std::size_t>(row)];
        graphics.setColour(textColour);
        graphics.setFont(WorkbenchType::labelStrong());
        graphics.drawFittedText(
            juceString(map.displayName),
            8,
            3,
            width - 16,
            16,
            juce::Justification::centredLeft,
            1);
        graphics.setColour(secondaryTextColour);
        graphics.setFont(WorkbenchType::micro());
        graphics.drawText(
            juce::String(static_cast<int>(map.regionCount))
                + (map.regionCount == 1 ? " region" : " regions"),
            8,
            19,
            width - 16,
            height - 20,
            juce::Justification::centredLeft);
    }

    SampleMapPanelCallbacks callbacks_;
    SampleMapPanelState state_;
    bool refreshing_ = false;
    double progressValue_ = 0.0;
    juce::Label title_;
    juce::Label status_;
    juce::ListBox list_;
    juce::TextEditor mapName_;
    juce::TextButton createButton_;
    juce::TextButton renameButton_;
    juce::TextButton removeButton_;
    juce::ComboBox disposition_;
    juce::TextButton importButton_;
    juce::TextEditor replaceConfirm_;
    juce::TextButton replaceButton_;
    juce::TextButton cancelButton_;
    juce::TextButton applyButton_;
    juce::Label lowLabel_;
    juce::Label anchorLabel_;
    juce::Label highLabel_;
    juce::ComboBox lowNote_;
    juce::ComboBox anchorNote_;
    juce::ComboBox highNote_;
    SampleMapCanvasComponent canvas_;
    juce::ProgressBar progress_{progressValue_};
};
}

std::unique_ptr<SampleMapPanel> makeSampleMapPanel(
    SampleMapPanelCallbacks callbacks)
{
    return std::make_unique<SampleMapPanelImpl>(std::move(callbacks));
}
}
