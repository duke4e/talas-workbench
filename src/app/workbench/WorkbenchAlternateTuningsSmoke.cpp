#include "app/workbench/WorkbenchComponentInternal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iterator>

namespace talas::app::workbench
{
// Tools > Design Alternate Tunings sets the
// tonic, the method and its parameters, lists the octave from the tonic,
// saves and loads tuning files and adds an example Sound to the project.
[[nodiscard]] std::optional<std::string>
WorkbenchComponent::runAlternateTuningsSmoke(const std::filesystem::path& smokeDirectory)
{
    namespace tunings = talas::project::tunings;
    const auto previousDocument = projectBridge_.editor().document();
    const auto previousProjectPath = projectPath();
    const auto previousCleanDocument = editBoundary_.cleanDocument();
    const auto prepared = soundBrowserUsesPreparedProfileForSmoke_;
    const auto finish = [&](std::string error = {}) -> std::optional<std::string> {
        stopTuningTryOut();
        soundBrowserUsesPreparedProfileForSmoke_ = prepared;
        const auto restored = projectBridge_.editor().replaceDocument(previousDocument);
        editBoundary_.markRecovered(previousCleanDocument, previousProjectPath);
        documentChanged();
        if (!restored.ok() && error.empty())
            error = "alternate tunings smoke could not restore the project";
        return error.empty() ? std::nullopt : std::optional<std::string>{std::move(error)};
    };
    talas::project::ProjectDocument fixture;
    fixture.expressionLanguage = talas::expr::ExpressionLanguageVersion::ValV1;
    talas::project::reconcileProjectPresentation(fixture);
    if (const auto replaced = projectBridge_.editor().replaceDocument(fixture); !replaced.ok())
        return finish("alternate tunings fixture was rejected: " + replaced.message);
    documentChanged();
    const auto names = getMenuBarNames();
    const auto toolsMenu = getMenuForIndex(names.indexOf("Tools"), "Tools");
    auto listed = false;
    soundBrowserUsesPreparedProfileForSmoke_ = true;
    for (juce::PopupMenu::MenuItemIterator item{toolsMenu}; item.next();)
        listed = listed || item.getItem().itemID == commandShowAlternateTunings;
    if (!listed || !commandManager_.invokeDirectly(commandShowAlternateTunings, false) || alternateTuningsPanel_ == nullptr)
        return finish("the Tools menu did not open Design Alternate Tunings");
    auto& panel = *alternateTuningsPanel_;
    // Open, the Tool plays its example for the MIDI keyboard
    // to try out; a key played sounds at once.
    if (tuningTryOutAudition_ == nullptr || soundBrowserAudition_.get() != tuningTryOutAudition_)
        return finish("opening the Tool did not play its tuning for the MIDI keyboard: " + statusLabel_.getText().toStdString());
    forwardMidiNoteToAudition(true, 1, 67, 100, 0.0);
    std::array<float, 64> left{};
    std::array<float, 64> right{};
    auto loudest = 0.0F;
    for (int block = 0; block < 40; ++block)
    {
        std::array<float*, 2> channels{left.data(), right.data()};
        left.fill(0.0F);
        right.fill(0.0F);
        (void)renderSoundBrowserAudition(1.0 + block * 0.008, channels.data(), channels.size(), left.size(), {});
        for (const auto sample : left)
            loudest = std::max(loudest, std::abs(sample));
    }
    forwardMidiNoteToAudition(false, 1, 67, 0, 0.0);
    if (loudest < 0.01F)
        return finish("a key played in the Tool did not sound its tuning");
    panel.setTuningForSmoke({});
    // Middle C at its equal-tempered frequency, by default.
    if (panel.tuningForSmoke().tonicKey != 60 || std::abs(panel.tuningForSmoke().tonicFrequency - 261.6255653005986) > 1e-9)
        return finish("the tonic should default to middle C at its equal-tempered frequency");
    // An equal-tempered scale's one parameter; the octave
    // from the tonic with each degree's frequency, ratio and cents.
    if (!panel.enterForSmoke("degrees", "19") || panel.tuningForSmoke().degreesPerOctave != 19)
        return finish("the degrees per octave field did not take 19");
    const auto octave = panel.octaveTextForSmoke();
    if (juce::StringArray::fromLines(octave.trimEnd()).size() != 20 || !octave.contains(" 61 ")
        || !octave.contains("63.158") || !octave.contains(" 78 "))
    {
        return finish("the octave view should list the 19 degrees from the tonic with their cents: " + octave.toStdString());
    }
    // A value typed is taken with Enter, Tab or a click in
    // another field, any of which leaves the field.
    if (!panel.leaveFieldForSmoke("degrees", "17") || panel.tuningForSmoke().degreesPerOctave != 17
        || !panel.enterForSmoke("degrees", "19"))
        return finish("leaving the degrees per octave field did not take 17");
    if (panel.enterForSmoke("tonicFrequency", "-3") || panel.tuningForSmoke().tonicFrequency <= 0.0)
        return finish("a tonic frequency below 0 hz was taken");
    // Save writes the 128 keys' frequencies; Load into another
    // method approximates the tuning, into its own takes its parameters.
    const auto file = smokeDirectory / "nineteen.txt";
    if (!panel.saveForSmoke(file))
        return finish("Save did not write the tuning file");
    std::ifstream saved(file, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>{saved}, std::istreambuf_iterator<char>{}};
    if (text.find("\n127 ") == std::string::npos || text.find("\n999\n") == std::string::npos)
        return finish("the tuning file should list every key's frequency and end with 999");
    auto ratio = panel.tuningForSmoke();
    ratio.method = tunings::Method::RatioScale;
    panel.setTuningForSmoke(ratio);
    if (!panel.loadForSmoke(file)
        || std::abs(panel.tuningForSmoke().ratios[1].numerator / panel.tuningForSmoke().ratios[1].denominator
               - std::exp2(1.0 / 19.0))
            > 1e-5)
    {
        return finish("loading the file into the ratio scale should approximate its tuning");
    }
    if (!panel.enterForSmoke("numerator8", "3") || !panel.enterForSmoke("denominator8", "2")
        || panel.tuningForSmoke().ratios[7] != tunings::Ratio{3, 2})
    {
        return finish("the ratio fields did not take 3/2");
    }
    // The example scales.
    if (!panel.presetForSmoke("Just") || panel.tuningForSmoke().ratios[4] != tunings::Ratio{5, 4}
        || !panel.presetForSmoke("Pythagorean") || panel.tuningForSmoke().ratios[4] != tunings::Ratio{81, 64}
        || panel.presetForSmoke("Meantone"))
        return finish("the Just and Pythagorean buttons did not preset the ratios");
    panel.setTuningForSmoke({});
    if (!panel.loadForSmoke(file) || panel.tuningForSmoke().degreesPerOctave != 19)
        return finish("loading the file into its own method should give back its 19 degrees");
    // Create Example adds a Sound playing the tuning from the MIDI keyboard,
    // a 21-harmonic sawtooth here, and its Volume fader.
    panel.chooseInstrumentForSmoke(talas::app::TuningExampleInstrument::Sawtooth);
    if (!panel.createExampleForSmoke())
        return finish("Create Example did not add the tuning's Sound");
    const auto& definitions = projectBridge_.editor().document().soundDefinitions;
    if (definitions.empty()
        || std::ranges::none_of(definitions.back().graph.nodes, [](const auto& node) { return node.typeId == "talas.midiVoice"; }))
    {
        return finish("the example should be a Sound of a MIDIVoice");
    }
    const auto sawtooth = std::ranges::any_of(definitions.back().graph.nodes, [](const talas::project::ProjectNode& node) {
        return std::ranges::any_of(node.parameters, [](const auto& parameter) {
            return parameter.resource == std::optional<std::string>{"Saw21"};
        });
    });
    const auto& controls = projectBridge_.editor().document().virtualControls;
    if (!sawtooth || std::ranges::none_of(controls, [](const auto& control) {
            return control.eventName == "Volume" && control.value == 1.0;
        }))
        return finish("the example should play the sawtooth at the Volume fader's level");
    // The fader's level, then the example.
    undoProject();
    undoProject();
    if (!projectBridge_.editor().document().soundDefinitions.empty())
        return finish("Undo did not remove the example and its Volume fader");
    return finish();
}
}
