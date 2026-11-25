#pragma once

#include "talas/app/AnalysisToolsModel.h"
#include "talas/project/AlternateTunings.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <filesystem>
#include <functional>
#include <memory>

namespace talas::app::workbench
{
// The Design Alternate Tunings Tool: the
// tonic key and frequency, the five ways to tune the keys and their
// parameters, the octave from the tonic with each degree's frequency, ratio
// and cents, Load and Save of tuning files, and Create Example, which adds
// a Sound playing the tuning from the MIDI keyboard.
class AlternateTuningsPanel : public juce::Component
{
public:
    using StatusCallback = std::function<void(const juce::String&)>;
    // Adds the example Sound to the project; false when it refused it.
    using ExampleCallback = std::function<bool(const talas::project::tunings::Tuning&,
        const talas::project::tunings::KeyFrequencies&, talas::app::TuningExampleInstrument, const std::filesystem::path&)>;

    // Plays the tuning for the MIDI keyboard to try out in real time while
    // the Tool is open: called at once, and
    // again whenever the tuning or the example's instrument changes.
    using TryOutCallback = std::function<void(const talas::project::tunings::Tuning&,
        const talas::project::tunings::KeyFrequencies&, talas::app::TuningExampleInstrument, const std::filesystem::path&)>;

    ~AlternateTuningsPanel() override = default;

    virtual void setTryOut(TryOutCallback tryOut) = 0;

    [[nodiscard]] virtual talas::project::tunings::Tuning tuningForSmoke() const = 0;
    virtual void setTuningForSmoke(const talas::project::tunings::Tuning& tuning) = 0;
    // Types a value into the named field and enters it, as the Tool's
    // fields take a value: "tonicKey", "tonicFrequency",
    // "degrees", "interval", "numerator1" to "numerator12",
    // "denominator1" to "denominator12" and "cents1" to "cents12".
    [[nodiscard]] virtual bool enterForSmoke(const juce::String& field, const juce::String& value) = 0;
    // A value typed and the field left, as Tab or a click in another field
    // leaves it: whether the tuning took it.
    [[nodiscard]] virtual bool leaveFieldForSmoke(const juce::String& field, const juce::String& value) = 0;
    [[nodiscard]] virtual juce::String octaveTextForSmoke() const = 0;
    [[nodiscard]] virtual bool saveForSmoke(const std::filesystem::path& file) = 0;
    [[nodiscard]] virtual bool loadForSmoke(const std::filesystem::path& file) = 0;
    [[nodiscard]] virtual bool createExampleForSmoke() = 0;
    // The radio buttons of the example's instrument, and the
    // buttons presetting the ratios or cents to an example scale:
    // "Equal", "Just", "Pythagorean", "Meantone", "Werckmeister".
    virtual void chooseInstrumentForSmoke(talas::app::TuningExampleInstrument instrument) = 0;
    [[nodiscard]] virtual bool presetForSmoke(const juce::String& example) = 0;
};

[[nodiscard]] std::unique_ptr<AlternateTuningsPanel> makeAlternateTuningsPanel(
    AlternateTuningsPanel::StatusCallback onStatus, AlternateTuningsPanel::ExampleCallback onExample);
}
