#pragma once

#include "talas/app/SpectrumSynchronization.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace talas::app::workbench
{
// The Synchronizing Spectra tool: its description, then the Guide
// spectrum above and
// the Warped spectrum below, each with its markers; AutoMarkers, the m key
// adding a marker at the scrub bar, Enter naming the selected one, Delete
// removing it, Tab and a click switching views, Space auditioning the
// active one; Save Markers; and Mix and Morph, which leave example Sounds.
class SynchronizingSpectraPanel : public juce::Component
{
public:
    enum class Example
    {
        Mix,
        Morph,
    };

    struct Completed
    {
        Example example = Example::Mix;
        std::string name;
        talas::app::SynchronizedSpectrum guide;
        talas::app::SynchronizedSpectrum warped;
    };

    using StatusCallback = std::function<void(const juce::String&)>;
    using CompletedCallback = std::function<void(Completed)>;
    // A mono buffer to play, looped or once; none to stop.
    using AuditionCallback = std::function<void(std::vector<double> samples, double sampleRate, bool loop)>;

    ~SynchronizingSpectraPanel() override = default;

    // Smoke access: Continue past the description, a spectrum chosen for a
    // view, a view's markers, a key pressed in a view, the scrub bar moved,
    // a marker selected, and Mix or Morph pressed.
    virtual bool continueForSmoke() = 0;
    [[nodiscard]] virtual bool onDescriptionForSmoke() const = 0;
    [[nodiscard]] virtual bool loadForSmoke(bool guide, const std::filesystem::path& spectrum) = 0;
    [[nodiscard]] virtual std::vector<talas::app::SpectrumMarker> markersForSmoke(bool guide) const = 0;
    virtual bool keyForSmoke(bool guide, const juce::KeyPress& key) = 0;
    virtual void scrubForSmoke(bool guide, std::size_t frame) = 0;
    virtual void selectMarkerForSmoke(bool guide, std::optional<std::size_t> marker) = 0;
    [[nodiscard]] virtual std::size_t scrubbedForSmoke(bool guide) const = 0;
    // MIDI pitch bend (0 to 16383) scrubs the active view's spectrum across
    // the file; whether there was one to scrub.
    virtual bool scrubForPitchBend(int bend) = 0;
    [[nodiscard]] virtual bool guideIsActiveForSmoke() const = 0;
    [[nodiscard]] virtual bool exampleForSmoke(Example example) = 0;
};

[[nodiscard]] std::unique_ptr<SynchronizingSpectraPanel> makeSynchronizingSpectraPanel(
    SynchronizingSpectraPanel::StatusCallback onStatus,
    SynchronizingSpectraPanel::CompletedCallback onCompleted,
    SynchronizingSpectraPanel::AuditionCallback onAudition);
}
