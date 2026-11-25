#include "app/workbench/WorkbenchSynchronizingSpectra.h"

#include "talas/app/AnalysisToolsModel.h"

#include "app/workbench/WorkbenchTheme.h"
#include "app/workbench/WorkbenchUiHelpers.h"
#include "talas/audio/PartialTrackResynthesis.h"
#include "talas/app/PartialTrackSpectrumEditing.h"
#include "talas/audio/SpectrumTables.h"
#include "talas/project/SosSpectrumFile.h"
#include "talas/project/ProjectPathEncoding.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iterator>
#include <utility>

namespace talas::app::workbench
{
namespace
{
using talas::app::SpectrumMarker;

[[nodiscard]] juce::String pathText(const std::filesystem::path& path)
{
    return juceString(talas::project::pathToUtf8(path));
}

// A letter key without Command, Ctrl or Alt, typed or pressed.
[[nodiscard]] bool isLetter(const juce::KeyPress& key, char letter)
{
    const auto upper = static_cast<int>(std::toupper(static_cast<unsigned char>(letter)));
    return !key.getModifiers().isAnyModifierKeyDown()
        && (key.getTextCharacter() == static_cast<juce::juce_wchar>(letter) || key.getKeyCode() == letter
            || key.getKeyCode() == upper);
}

[[nodiscard]] talas::app::SpectrumSyncKey syncKey(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::tabKey)
        return talas::app::SpectrumSyncKey::Tab;
    if (key == juce::KeyPress::spaceKey)
        return talas::app::SpectrumSyncKey::Space;
    if (key == juce::KeyPress::leftKey)
        return talas::app::SpectrumSyncKey::Left;
    if (key == juce::KeyPress::rightKey)
        return talas::app::SpectrumSyncKey::Right;
    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
        return talas::app::SpectrumSyncKey::Delete;
    if (key == juce::KeyPress::returnKey)
        return talas::app::SpectrumSyncKey::Return;
    if (isLetter(key, 'm'))
        return talas::app::SpectrumSyncKey::M;
    if (isLetter(key, 'a'))
        return talas::app::SpectrumSyncKey::A;
    if (isLetter(key, 's'))
        return talas::app::SpectrumSyncKey::S;
    return talas::app::SpectrumSyncKey::Other;
}

[[nodiscard]] juce::Colour markerColour(std::size_t index, bool selected)
{
    switch (talas::app::spectrumMarkerColour(index, selected))
    {
    case talas::app::SpectrumMarkerColour::White:
        return juce::Colours::white;
    case talas::app::SpectrumMarkerColour::Red:
        return juce::Colours::red;
    case talas::app::SpectrumMarkerColour::Green:
        return juce::Colours::limegreen;
    case talas::app::SpectrumMarkerColour::Blue:
        return juce::Colour{0xff4f8cff};
    }
    return juce::Colours::red;
}

// A spectrum with its markers above it and the yellow scrub bar, time
// across and frequency up, amplitude as colour. The active view has a white
// border. A click in the marker strip selects the marker there and moves
// the scrub bar to it, or deselects; a click or drag in the spectrum moves
// the scrub bar; Ctrl or Command and a box zooms in.
class SpectrumMarkerView final : public juce::Component
{
public:
    std::function<void()> onActivated;
    std::function<void(std::size_t frame)> onScrubbed;

    void setSpectrum(const talas::project::PartialTrackAsset* spectrum)
    {
        spectrum_ = spectrum;
        selected_.reset();
        scrub_ = 0;
        zoomOut();
    }

    void setMarkers(std::vector<SpectrumMarker> markers)
    {
        markers_ = std::move(markers);
        if (selected_ && *selected_ >= markers_.size())
            selected_.reset();
        repaint();
    }

    [[nodiscard]] const std::vector<SpectrumMarker>& markers() const noexcept { return markers_; }
    [[nodiscard]] std::optional<std::size_t> selected() const noexcept { return selected_; }
    void select(std::optional<std::size_t> marker)
    {
        selected_ = marker && *marker < markers_.size() ? marker : std::nullopt;
        if (selected_)
            scrub_ = markers_[*selected_].frame;
        repaint();
    }
    [[nodiscard]] std::size_t scrub() const noexcept { return scrub_; }
    void setScrub(std::size_t frame)
    {
        scrub_ = spectrum_ == nullptr || spectrum_->frames.empty() ? 0 : std::min(frame, spectrum_->frames.size() - 1);
        repaint();
    }
    void setActive(bool active)
    {
        active_ = active;
        repaint();
    }

    void zoomOut()
    {
        lowFrame_ = 0;
        highFrame_ = spectrum_ == nullptr ? 1 : std::max<std::size_t>(1, spectrum_->frames.size());
        highHz_ = 0.0;
        if (spectrum_ != nullptr)
        {
            for (const auto& frame : spectrum_->frames)
            {
                for (const auto& point : frame.points)
                {
                    if (point.amplitude > 0.0)
                        highHz_ = std::max(highHz_, point.frequencyHz);
                }
            }
        }
        highHz_ = std::clamp(highHz_ * 1.05, 500.0, 20000.0);
        lowHz_ = 0.0;
        image_ = {};
        repaint();
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(juce::Colours::black);
        const auto area = spectrumArea();
        if (spectrum_ != nullptr && !spectrum_->frames.empty())
        {
            if (!image_.isValid() || image_.getWidth() != area.getWidth() || image_.getHeight() != area.getHeight())
                renderSpectrum(area);
            graphics.drawImageAt(image_, area.getX(), area.getY());
        }
        const auto strip = markerStrip();
        graphics.setColour(workbenchTheme.surface);
        graphics.fillRect(strip);
        for (std::size_t index = 0; index < markers_.size(); ++index)
        {
            const auto x = frameX(markers_[index].frame);
            const auto colour = markerColour(index, selected_ == index);
            graphics.setColour(colour);
            graphics.drawVerticalLine(static_cast<int>(x), static_cast<float>(strip.getY()), static_cast<float>(getHeight()));
            graphics.fillRect(juce::Rectangle<float>{x - 3.0F, static_cast<float>(strip.getY()), 6.0F, 6.0F});
            graphics.setFont(WorkbenchType::micro());
            graphics.drawText(juceString(markers_[index].name),
                juce::Rectangle<float>{x + 4.0F, static_cast<float>(strip.getY()), 80.0F, static_cast<float>(strip.getHeight())},
                juce::Justification::centredLeft, false);
        }
        graphics.setColour(juce::Colours::yellow);
        graphics.drawVerticalLine(static_cast<int>(frameX(scrub_)), static_cast<float>(area.getY()), static_cast<float>(getHeight()));
        if (zoomBox_)
        {
            graphics.setColour(juce::Colours::yellow.withAlpha(0.6F));
            graphics.drawRect(*zoomBox_, 1);
        }
        graphics.setColour(active_ ? juce::Colours::white : workbenchTheme.outline);
        graphics.drawRect(getLocalBounds(), active_ ? 2 : 1);
    }

    void resized() override { image_ = {}; }

    void mouseDown(const juce::MouseEvent& event) override
    {
        if (onActivated)
            onActivated();
        grabKeyboardFocus();
        start_ = event.getPosition();
        if (event.mods.isCtrlDown() || event.mods.isCommandDown())
        {
            zoomBox_ = juce::Rectangle<int>{start_, start_};
            return;
        }
        if (markerStrip().contains(event.getPosition()))
        {
            selected_.reset();
            for (std::size_t index = 0; index < markers_.size(); ++index)
            {
                if (std::abs(frameX(markers_[index].frame) - event.position.x) <= 5.0F)
                    select(index);
            }
            repaint();
            return;
        }
        scrubTo(event.position.x);
    }

    void mouseDrag(const juce::MouseEvent& event) override
    {
        if (zoomBox_)
        {
            zoomBox_ = juce::Rectangle<int>::leftTopRightBottom(std::min(start_.x, event.x), std::min(start_.y, event.y),
                std::max(start_.x, event.x), std::max(start_.y, event.y));
            repaint();
            return;
        }
        if (!markerStrip().contains(start_))
            scrubTo(event.position.x);
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        if (!zoomBox_)
            return;
        const auto box = zoomBox_->getIntersection(spectrumArea());
        zoomBox_.reset();
        const auto area = spectrumArea().toDouble();
        const auto fraction = [](double at, double start, double size) { return size > 0.0 ? (at - start) / size : 0.0; };
        if (const auto window = box.getWidth() > 4 && box.getHeight() > 4
                ? talas::app::spectrumViewZoomedTo({static_cast<double>(lowFrame_), static_cast<double>(highFrame_), lowHz_, highHz_},
                      fraction(box.getX(), area.getX(), area.getWidth()), fraction(box.getY(), area.getY(), area.getHeight()),
                      fraction(box.getRight(), area.getX(), area.getWidth()), fraction(box.getBottom(), area.getY(), area.getHeight()))
                : std::nullopt)
        {
            const auto left = static_cast<std::size_t>(std::lround(std::max(0.0, window->lowTime)));
            const auto right = static_cast<std::size_t>(std::lround(std::max(0.0, window->highTime)));
            lowFrame_ = left;
            highFrame_ = std::max(left + 1, right);
            lowHz_ = std::max(0.0, window->lowHz);
            highHz_ = std::max(lowHz_ + 1.0, window->highHz);
            image_ = {};
        }
        repaint();
    }

private:
    [[nodiscard]] juce::Rectangle<int> markerStrip() const { return getLocalBounds().reduced(2).removeFromTop(16); }
    [[nodiscard]] juce::Rectangle<int> spectrumArea() const
    {
        auto area = getLocalBounds().reduced(2);
        area.removeFromTop(16);
        return area;
    }

    [[nodiscard]] float frameX(std::size_t frame) const
    {
        const auto area = spectrumArea().toFloat();
        const auto span = static_cast<float>(std::max<std::size_t>(1, highFrame_ - lowFrame_));
        return area.getX() + (static_cast<float>(frame) - static_cast<float>(lowFrame_)) / span * area.getWidth();
    }

    [[nodiscard]] std::size_t frameAt(float x) const
    {
        const auto area = spectrumArea().toFloat();
        const auto amount = std::clamp((x - area.getX()) / std::max(1.0F, area.getWidth()), 0.0F, 1.0F);
        return lowFrame_ + static_cast<std::size_t>(std::lround(amount * static_cast<float>(highFrame_ - lowFrame_)));
    }

    void scrubTo(float x)
    {
        setScrub(frameAt(x));
        if (onScrubbed)
            onScrubbed(scrub_);
    }

    void renderSpectrum(juce::Rectangle<int> area)
    {
        image_ = juce::Image{juce::Image::RGB, std::max(1, area.getWidth()), std::max(1, area.getHeight()), true};
        juce::Graphics graphics{image_};
        double loudest = 0.0;
        for (const auto& frame : spectrum_->frames)
        {
            for (const auto& point : frame.points)
                loudest = std::max(loudest, point.amplitude);
        }
        if (!(loudest > 0.0))
            return;
        const auto width = static_cast<float>(area.getWidth());
        const auto height = static_cast<double>(area.getHeight());
        const auto span = static_cast<float>(std::max<std::size_t>(1, highFrame_ - lowFrame_));
        for (auto index = lowFrame_; index < highFrame_ && index < spectrum_->frames.size(); ++index)
        {
            const auto left = (static_cast<float>(index - lowFrame_)) / span * width;
            const auto right = std::max(left + 1.0F, (static_cast<float>(index + 1 - lowFrame_)) / span * width);
            for (const auto& point : spectrum_->frames[index].points)
            {
                if (point.frequencyHz < lowHz_ || point.frequencyHz > highHz_ || !(point.amplitude > 0.0))
                    continue;
                const auto level = std::clamp(1.0 + 20.0 * std::log10(point.amplitude / loudest) / 60.0, 0.0, 1.0);
                graphics.setColour(level < 0.5
                        ? juce::Colour::fromFloatRGBA(0.0F, static_cast<float>(level * 2.0), static_cast<float>(0.5 - level), 1.0F)
                        : juce::Colour::fromFloatRGBA(static_cast<float>((level - 0.5) * 2.0), 1.0F, 0.0F, 1.0F));
                const auto y = static_cast<float>(height - (point.frequencyHz - lowHz_) / (highHz_ - lowHz_) * height);
                graphics.fillRect(juce::Rectangle<float>{left, y - 1.0F, right - left, 2.0F});
            }
        }
    }

    const talas::project::PartialTrackAsset* spectrum_ = nullptr;
    std::vector<SpectrumMarker> markers_;
    std::optional<std::size_t> selected_;
    std::size_t scrub_ = 0;
    bool active_ = false;
    std::size_t lowFrame_ = 0;
    std::size_t highFrame_ = 1;
    double lowHz_ = 0.0;
    double highHz_ = 2000.0;
    juce::Point<int> start_;
    std::optional<juce::Rectangle<int>> zoomBox_;
    juce::Image image_;
};

// One of the two spectra: its file, its frames and markers, and its
// resynthesis for the audition.
struct LoadedSpectrum
{
    std::filesystem::path file;
    talas::project::PartialTrackAsset asset;
    std::vector<double> resynthesis;
};

class SynchronizingSpectraPanelImpl final : public SynchronizingSpectraPanel
{
public:
    SynchronizingSpectraPanelImpl(StatusCallback onStatus, CompletedCallback onCompleted, AuditionCallback onAudition)
        : onStatus_(std::move(onStatus)), onCompleted_(std::move(onCompleted)), onAudition_(std::move(onAudition))
    {
        setOpaque(true);
        setWantsKeyboardFocus(true);
        heading_.setText("Synchronizing Spectra", juce::dontSendNotification);
        heading_.setFont(WorkbenchType::panelTitle());
        addAndMakeVisible(heading_);
        description_.setText(
            "Synchronize two spectra so that corresponding events can be morphed to each other. Choose a Guide "
            "spectrum and a Warped one, then place markers at the times where the two should line up: the same "
            "number in each, at the beginning, the end, and the beginnings and ends of events. The Warped "
            "spectrum's time is sped up or slowed down to bring its markers to the Guide's. Mix plays the two "
            "resynthesized, one in each channel, to check them; Morph morphs one into the other.",
            juce::dontSendNotification);
        description_.setJustificationType(juce::Justification::topLeft);
        addAndMakeVisible(description_);
        continue_.setButtonText("Continue");
        continue_.onClick = [this] { (void) continueForSmoke(); };
        addAndMakeVisible(continue_);

        for (auto* button : {&guideFile_, &warpedFile_, &guideAuto_, &warpedAuto_, &save_, &mix_, &morph_})
            addChildComponent(*button);
        guideFile_.setButtonText("Guide...");
        warpedFile_.setButtonText("Warped...");
        guideAuto_.setButtonText("AutoMarkers");
        warpedAuto_.setButtonText("AutoMarkers");
        save_.setButtonText("Save Markers");
        mix_.setButtonText("Mix");
        morph_.setButtonText("Morph");
        guideFile_.onClick = [this] { choose(true); };
        warpedFile_.onClick = [this] { choose(false); };
        guideAuto_.onClick = [this] { autoMarkers(true); };
        warpedAuto_.onClick = [this] { autoMarkers(false); };
        save_.onClick = [this] {
            saveMarkers(true);
            saveMarkers(false);
        };
        mix_.onClick = [this] { (void) exampleForSmoke(Example::Mix); };
        morph_.onClick = [this] { (void) exampleForSmoke(Example::Morph); };
        for (const auto guide : {true, false})
        {
            auto& view = this->view(guide);
            addChildComponent(view);
            view.setWantsKeyboardFocus(true);
            view.addKeyListener(&keys_);
            view.onActivated = [this, guide] { activate(guide); };
            view.onScrubbed = [this, guide](std::size_t frame) { scrubAudition(guide, frame); };
            auto& label = this->label(guide);
            label.setText(guide ? "Guide" : "Warped", juce::dontSendNotification);
            addChildComponent(label);
        }
        status_.setColour(juce::Label::textColourId, secondaryTextColour);
        addAndMakeVisible(status_);
        keys_.owner = this;
        addKeyListener(&keys_);
        setSize(760, 560);
    }

    ~SynchronizingSpectraPanelImpl() override
    {
        if (onAudition_)
            onAudition_({}, 0.0, false);
    }

    void paint(juce::Graphics& graphics) override { graphics.fillAll(panelColour); }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(12);
        heading_.setBounds(bounds.removeFromTop(24));
        bounds.removeFromTop(6);
        status_.setBounds(bounds.removeFromBottom(22));
        if (description_.isVisible())
        {
            continue_.setBounds(bounds.removeFromBottom(28).removeFromRight(100));
            description_.setBounds(bounds);
            return;
        }
        auto buttons = bounds.removeFromBottom(28);
        morph_.setBounds(buttons.removeFromRight(80));
        buttons.removeFromRight(6);
        mix_.setBounds(buttons.removeFromRight(80));
        buttons.removeFromRight(6);
        save_.setBounds(buttons.removeFromRight(120));
        warpedFile_.setBounds(buttons.removeFromLeft(100));
        buttons.removeFromLeft(6);
        guideFile_.setBounds(buttons.removeFromLeft(100));
        bounds.removeFromBottom(6);
        const auto half = bounds.getHeight() / 2;
        for (const auto guide : {true, false})
        {
            auto area = bounds.removeFromTop(half);
            auto top = area.removeFromTop(24);
            label(guide).setBounds(top.removeFromLeft(80));
            (guide ? guideAuto_ : warpedAuto_).setBounds(top.removeFromLeft(110));
            area.removeFromTop(4);
            view(guide).setBounds(area.reduced(0, 2));
        }
    }

    bool continueForSmoke() override
    {
        if (!description_.isVisible())
            return false;
        // Continue puts the spectra in the instructions' place.
        using Part = talas::app::SynchronizingSpectraPart;
        description_.setVisible(talas::app::synchronizingSpectraShows(true, Part::Instructions));
        continue_.setVisible(talas::app::synchronizingSpectraShows(true, Part::ContinueButton));
        for (auto* component : std::initializer_list<juce::Component*>{&guideFile_, &warpedFile_, &guideAuto_,
                 &warpedAuto_, &save_, &mix_, &morph_, &guideView_, &warpedView_, &guideLabel_, &warpedLabel_})
            component->setVisible(talas::app::synchronizingSpectraShows(true, Part::Spectra));
        activate(true);
        resized();
        status_.setText("Choose the Guide and Warped spectra", juce::dontSendNotification);
        return true;
    }

    [[nodiscard]] bool onDescriptionForSmoke() const override { return description_.isVisible(); }

    [[nodiscard]] bool loadForSmoke(bool guide, const std::filesystem::path& spectrum) override
    {
        return load(guide, spectrum);
    }

    [[nodiscard]] std::vector<SpectrumMarker> markersForSmoke(bool guide) const override
    {
        return guide ? guideView_.markers() : warpedView_.markers();
    }

    bool keyForSmoke(bool guide, const juce::KeyPress& key) override
    {
        activate(guide);
        return handleKey(key);
    }

    void scrubForSmoke(bool guide, std::size_t frame) override { view(guide).setScrub(frame); }
    void selectMarkerForSmoke(bool guide, std::optional<std::size_t> marker) override { view(guide).select(marker); }
    bool scrubForPitchBend(int bend) override
    {
        const auto& spectrum = loaded(guideActive_);
        if (!spectrum || spectrum->asset.frames.empty())
            return false;
        const auto frame = talas::app::partialTrackScrubForPitchBend(bend, spectrum->asset.frames.size());
        view(guideActive_).setScrub(static_cast<std::size_t>(frame));
        scrubAudition(guideActive_, view(guideActive_).scrub());
        return true;
    }

    [[nodiscard]] std::size_t scrubbedForSmoke(bool guide) const override
    {
        return guide ? guideView_.scrub() : warpedView_.scrub();
    }
    [[nodiscard]] bool guideIsActiveForSmoke() const override { return guideActive_; }

    [[nodiscard]] bool exampleForSmoke(Example example) override
    {
        if (!guide_ || !warped_)
        {
            status("Choose the Guide and Warped spectra first");
            return false;
        }
        if (!talas::app::spectrumMarkersCorrespond(guideView_.markers(), warpedView_.markers()))
        {
            status("The two spectra need the same number of markers, two at least");
            return false;
        }
        const auto prepared = [this](const LoadedSpectrum& loaded, const std::vector<SpectrumMarker>& markers)
            -> std::optional<talas::app::SynchronizedSpectrum> {
            const auto table = talas::audio::spectrumInRamTable(loaded.asset);
            const auto path = talas::app::spectrumTablePath(loaded.file);
            std::string error;
            if (!talas::audio::writeSpectrumInRamTable(path, table, error))
            {
                status("The SpectrumInRAM table could not be written: " + juceString(error));
                return std::nullopt;
            }
            return talas::app::SynchronizedSpectrum{
                path, table.frames, table.partials, talas::app::spectrumTimePoints(loaded.asset, markers)};
        };
        auto guide = prepared(*guide_, guideView_.markers());
        auto warped = guide ? prepared(*warped_, warpedView_.markers()) : std::nullopt;
        if (!guide || !warped)
            return false;
        Completed completed{example,
            talas::project::pathToUtf8(warped_->file.stem()) + " to " + talas::project::pathToUtf8(guide_->file.stem()),
            std::move(*guide), std::move(*warped)};
        if (onCompleted_)
            onCompleted_(std::move(completed));
        return true;
    }

private:
    struct Keys final : juce::KeyListener
    {
        SynchronizingSpectraPanelImpl* owner = nullptr;
        bool keyPressed(const juce::KeyPress& key, juce::Component*) override { return owner->handleKey(key); }
    };

    [[nodiscard]] SpectrumMarkerView& view(bool guide) { return guide ? guideView_ : warpedView_; }
    [[nodiscard]] juce::Label& label(bool guide) { return guide ? guideLabel_ : warpedLabel_; }
    [[nodiscard]] std::optional<LoadedSpectrum>& loaded(bool guide) { return guide ? guide_ : warped_; }

    void status(const juce::String& text)
    {
        status_.setText(text, juce::dontSendNotification);
        if (onStatus_)
            onStatus_(text);
    }

    void activate(bool guide)
    {
        guideActive_ = guide;
        guideView_.setActive(guide);
        warpedView_.setActive(!guide);
    }

    // The keys in the active view.
    bool handleKey(const juce::KeyPress& key)
    {
        if (description_.isVisible())
            return false;
        auto& active = view(guideActive_);
        auto& spectrum = loaded(guideActive_);
        const auto pressed = syncKey(key);
        switch (talas::app::spectrumSyncAction(pressed, spectrum.has_value(), active.selected().has_value()))
        {
        case talas::app::SpectrumSyncAction::SwitchView:
            activate(!guideActive_);
            return true;
        case talas::app::SpectrumSyncAction::ScrubBack:
        case talas::app::SpectrumSyncAction::ScrubOn:
        {
            // The arrow keys move the scrub bar a frame.
            const auto scrub = active.scrub();
            active.setScrub(pressed == talas::app::SpectrumSyncKey::Left ? (scrub == 0 ? 0 : scrub - 1) : scrub + 1);
            scrubAudition(guideActive_, active.scrub());
            return true;
        }
        case talas::app::SpectrumSyncAction::Audition:
            if (onAudition_)
                onAudition_(spectrum->resynthesis, spectrum->asset.sampleRate, false);
            return true;
        case talas::app::SpectrumSyncAction::AddMarker:
            if (auto added = talas::app::spectrumMarkersWith(active.markers(), active.scrub()))
                active.setMarkers(std::move(*added));
            return true;
        case talas::app::SpectrumSyncAction::SuggestMarkers:
            autoMarkers(guideActive_);
            return true;
        case talas::app::SpectrumSyncAction::SaveMarkers:
            saveMarkers(guideActive_);
            return true;
        case talas::app::SpectrumSyncAction::DeleteMarker:
            if (auto removed = talas::app::spectrumMarkersWithout(active.markers(), *active.selected()))
                active.setMarkers(std::move(*removed));
            return true;
        case talas::app::SpectrumSyncAction::NameMarker:
            askName(guideActive_, *active.selected());
            return true;
        case talas::app::SpectrumSyncAction::ShowWhole:
            active.zoomOut();
            return true;
        case talas::app::SpectrumSyncAction::None:
            break;
        }
        // Space is the tool's even with no spectrum to audition.
        return pressed == talas::app::SpectrumSyncKey::Space;
    }

    void choose(bool guide)
    {
        chooser_ = std::make_unique<juce::FileChooser>(guide ? "Choose the Guide Spectrum" : "Choose the Warped Spectrum",
            juce::File::getSpecialLocation(juce::File::userMusicDirectory),
            juce::String{std::string{talas::project::spectrumFilePatterns}});
        juce::Component::SafePointer<SynchronizingSpectraPanelImpl> safeThis(this);
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [safeThis, guide](const juce::FileChooser& chooser) {
                if (safeThis != nullptr && chooser.getResult() != juce::File{})
                    (void) safeThis->load(guide, talas::project::pathFromUtf8(chooser.getResult().getFullPathName().toStdString()));
            });
    }

    bool load(bool guide, const std::filesystem::path& file)
    {
        std::ifstream input(file, std::ios::binary);
        const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
        auto decoded = talas::project::decodePartialTrackAsset(bytes);
        if (!decoded.ok())
        {
            status("The spectrum could not be read: " + pathText(file.filename()));
            return false;
        }
        auto rendered = talas::audio::renderPartialTrackAssetOffline(decoded.asset);
        loaded(guide) = LoadedSpectrum{file, std::move(decoded.asset), rendered.ok() ? std::move(rendered.samples) : std::vector<double>{}};
        auto& shown = view(guide);
        shown.setSpectrum(&loaded(guide)->asset);
        // Markers the file already has are shown with it.
        std::string error;
        auto markers = talas::app::readSpectrumMarkers(file, error);
        shown.setMarkers(markers ? std::move(*markers) : std::vector<SpectrumMarker>{});
        label(guide).setText((guide ? "Guide: " : "Warped: ") + pathText(file.filename()), juce::dontSendNotification);
        activate(guide);
        status(error.empty() ? juce::String{"Place markers at corresponding points: a suggests them, m adds one"}
                             : juceString(error));
        return true;
    }

    void autoMarkers(bool guide)
    {
        if (const auto& spectrum = loaded(guide))
            view(guide).setMarkers(talas::app::autoSpectrumMarkers(spectrum->asset));
    }

    void saveMarkers(bool guide)
    {
        const auto& spectrum = loaded(guide);
        if (!spectrum)
            return;
        std::string error;
        // The cursor is a pen while the file is written.
        if (const auto pen = workbenchActivityMouseCursor(talas::app::WorkbenchCursor::WritingFile))
            setMouseCursor(*pen);
        juce::Desktop::getInstance().getMainMouseSource().forceMouseCursorUpdate();
        const auto written = talas::app::writeSpectrumMarkers(spectrum->file, view(guide).markers(), error);
        setMouseCursor(juce::MouseCursor::NormalCursor);
        juce::Desktop::getInstance().getMainMouseSource().forceMouseCursorUpdate();
        if (written)
        {
            // The resynthesis plays to show the markers are saved.
            status("Saved the markers of " + pathText(spectrum->file.filename()));
            if (onAudition_ && !spectrum->resynthesis.empty())
                onAudition_(spectrum->resynthesis, spectrum->asset.sampleRate, false);
        }
        else
            status(juceString(error));
    }

    // Dragging the scrub bar plays the resynthesis around it.
    void scrubAudition(bool guide, std::size_t frame)
    {
        const auto& spectrum = loaded(guide);
        if (!spectrum || !onAudition_ || spectrum->resynthesis.empty())
            return;
        const auto [begin, end] = talas::app::spectrumScrubSlice(spectrum->asset, frame, spectrum->resynthesis.size());
        onAudition_({spectrum->resynthesis.begin() + static_cast<std::ptrdiff_t>(begin),
                        spectrum->resynthesis.begin() + static_cast<std::ptrdiff_t>(end)},
            spectrum->asset.sampleRate, false);
    }

    void askName(bool guide, std::size_t marker)
    {
        nameWindow_ = std::make_unique<juce::AlertWindow>("Marker name", "Name the selected marker",
            juce::MessageBoxIconType::NoIcon, this);
        nameWindow_->addTextEditor("name", juceString(view(guide).markers()[marker].name));
        nameWindow_->addButton("OK", 1, juce::KeyPress{juce::KeyPress::returnKey});
        nameWindow_->addButton("Cancel", 0, juce::KeyPress{juce::KeyPress::escapeKey});
        juce::Component::SafePointer<SynchronizingSpectraPanelImpl> safeThis(this);
        nameWindow_->enterModalState(true, juce::ModalCallbackFunction::create([safeThis, guide, marker](int result) {
            if (safeThis == nullptr || result != 1)
                return;
            const auto name = safeThis->nameWindow_->getTextEditorContents("name").toStdString();
            if (auto renamed = talas::app::spectrumMarkersRenamed(safeThis->view(guide).markers(), marker, name))
                safeThis->view(guide).setMarkers(std::move(*renamed));
        }));
    }

    StatusCallback onStatus_;
    CompletedCallback onCompleted_;
    AuditionCallback onAudition_;
    std::optional<LoadedSpectrum> guide_;
    std::optional<LoadedSpectrum> warped_;
    bool guideActive_ = true;
    Keys keys_;
    std::unique_ptr<juce::FileChooser> chooser_;
    std::unique_ptr<juce::AlertWindow> nameWindow_;
    juce::Label heading_;
    juce::Label description_;
    juce::TextButton continue_;
    juce::TextButton guideFile_;
    juce::TextButton warpedFile_;
    juce::TextButton guideAuto_;
    juce::TextButton warpedAuto_;
    juce::TextButton save_;
    juce::TextButton mix_;
    juce::TextButton morph_;
    SpectrumMarkerView guideView_;
    SpectrumMarkerView warpedView_;
    juce::Label guideLabel_;
    juce::Label warpedLabel_;
    juce::Label status_;
};
}

std::unique_ptr<SynchronizingSpectraPanel> makeSynchronizingSpectraPanel(
    SynchronizingSpectraPanel::StatusCallback onStatus,
    SynchronizingSpectraPanel::CompletedCallback onCompleted,
    SynchronizingSpectraPanel::AuditionCallback onAudition)
{
    return std::make_unique<SynchronizingSpectraPanelImpl>(std::move(onStatus), std::move(onCompleted), std::move(onAudition));
}
}
