#include "talas/app/SoundIcons.h"
#include "talas/core/BuiltinNodes.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// The outer shapes of the Sound icons.
namespace
{
using talas::app::IconEdge;
using talas::app::SoundIconShape;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

SoundIconShape shapeOf(const talas::core::NodeRegistry& registry, std::string_view typeId)
{
    const auto* descriptor = registry.findNodeType(typeId);
    if (descriptor == nullptr)
        throw std::runtime_error("no such Sound: " + std::string{typeId});
    return talas::app::soundIconShape(*descriptor);
}

// Audio processors are rounded on both sides, sources flat on the left,
// envelopes flat on the left with a small rounded right edge.
void testAudioSourcesAndEnvelopes()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    require(shapeOf(registry, "talas.vcf") == SoundIconShape{IconEdge::Audio, IconEdge::Audio, false}
            && shapeOf(registry, "talas.chopper") == SoundIconShape{IconEdge::Audio, IconEdge::Audio, false},
        "an audio processor takes and gives audio");
    require(shapeOf(registry, "talas.oscillator") == SoundIconShape{IconEdge::None, IconEdge::Audio, false}
            && shapeOf(registry, "talas.sample") == SoundIconShape{IconEdge::None, IconEdge::Audio, false},
        "a sound source has no audio input, its modulator a control of its frequency");
    require(shapeOf(registry, "talas.adsr") == SoundIconShape{IconEdge::None, IconEdge::Subaudio, false}
            && shapeOf(registry, "talas.graphicalEnvelope").right == IconEdge::Subaudio,
        "an envelope generates a subaudio signal");
}

// Spectral sources give triangular outputs, the banks take triangular
// inputs, the modifiers both.
void testSpectra()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    require(shapeOf(registry, "talas.spectrumInRam") == SoundIconShape{IconEdge::None, IconEdge::Spectrum, false}
            && shapeOf(registry, "talas.liveSpectralAnalysis") == SoundIconShape{IconEdge::Audio, IconEdge::Spectrum, false},
        "a spectral source gives a spectrum");
    require(shapeOf(registry, "talas.oscillatorBank") == SoundIconShape{IconEdge::Spectrum, IconEdge::Audio, false}
            && shapeOf(registry, "talas.filterBank").left == IconEdge::Spectrum,
        "the additive and aggregate banks take a spectrum and give audio");
    require(shapeOf(registry, "talas.spectrumFrequencyScale") == SoundIconShape{IconEdge::Spectrum, IconEdge::Spectrum, false},
        "a spectral modifier takes and gives a spectrum");
}

// Side effects point up; environments point left and pass on what they
// take; constructors and variables give a variable output.
void testSideEffectsEnvironmentsAndConstructors()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    require(shapeOf(registry, "talas.oscilloscopeDisplay") == SoundIconShape{IconEdge::Audio, IconEdge::Transparent, true}
            && shapeOf(registry, "talas.spectrumAnalyzerDisplay") == SoundIconShape{IconEdge::Audio, IconEdge::Transparent, true},
        "a display has a visual side effect and is transparent to the signal");
    require(shapeOf(registry, "talas.midiOutputEvent").pointedTop && shapeOf(registry, "talas.diskRecorder").pointedTop,
        "MIDI output and a recording are side effects");
    require(shapeOf(registry, "talas.analogSequencer") == SoundIconShape{IconEdge::Environment, IconEdge::Transparent, false}
            && shapeOf(registry, "talas.midiVoice").left == IconEdge::Environment,
        "an AnalogSequencer supplies controls to its input and passes it on");
    require(shapeOf(registry, "talas.script") == SoundIconShape{IconEdge::Constructor, IconEdge::Variable, false}
            && shapeOf(registry, "talas.textFileInterpreter").left == IconEdge::Constructor,
        "a Script constructs a Sound out of those to its left");
    require(shapeOf(registry, "talas.variable").right == IconEdge::Variable, "a variable stands for an unknown Sound");
    require(talas::app::iconEdgeName(IconEdge::Transparent) == "rounded, small, double"
            && talas::app::iconEdgeName(IconEdge::Spectrum) == "triangular",
        "the edges are named as the book's table names them");
}
}

// The inner icons: the book's examples, and what each element and colour
// means.
void testInnerIcons()
{
    using talas::app::IconColour;
    using talas::app::IconElement;
    using talas::app::IconGlyph;
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto glyphs = [&](std::string_view typeId) {
        return talas::app::soundIconGlyphs(*registry.findNodeType(typeId));
    };
    require(glyphs("talas.sumOfSines") == std::vector<IconGlyph>{{IconElement::Oscillator}, {IconElement::Several},
                {IconElement::File}, {IconElement::Spectrum}}
            && shapeOf(registry, "talas.sumOfSines") == SoundIconShape{IconEdge::None, IconEdge::Audio, false},
        "a SumOfSines: oscillators, more than one, a file and a spectrum; no input and an audio output");
    require(glyphs("talas.filterBank") == std::vector<IconGlyph>{{IconElement::Bandpass, IconColour::Frequency}, {IconElement::Several}},
        "a FilterBank: a bandpass element in frequency's rose, more than one");
    require(glyphs("talas.memoryWriter") == std::vector<IconGlyph>{{IconElement::Ram}, {IconElement::Write}}
            && shapeOf(registry, "talas.memoryWriter") == SoundIconShape{IconEdge::Audio, IconEdge::Transparent, true},
        "a MemoryWriter: RAM and the hand with a pencil, transparent with a side effect");
    require(glyphs("talas.amplitudeFollower") == std::vector<IconGlyph>{{IconElement::Audio}, {IconElement::Output, IconColour::Amplitude}}
            && shapeOf(registry, "talas.amplitudeFollower") == SoundIconShape{IconEdge::Audio, IconEdge::Subaudio, false},
        "an AmplitudeFollower: audio and an amplitude output in blue green, audio in and subaudio out");
    require(talas::app::iconElementMeaning(IconElement::File) == "File" && talas::app::iconElementMeaning(IconElement::Ram) == "RAM"
            && talas::app::iconElementMeaning(IconElement::Cloud) == "Grain cloud"
            && talas::app::iconElementMeaning(IconElement::Output) == "Output or result"
            && talas::app::iconElementMeaning(IconElement::Several) == "More than one"
            && talas::app::iconElementMeaning(IconElement::Enabling) == "Transparent, enabling"
            && talas::app::iconElementMeaning(IconElement::Cycle) == "Cycle, sequence"
            && talas::app::iconElementMeaning(IconElement::Formants) == "Process formants",
        "the elements mean what the book's tables say");
    const std::vector<std::pair<IconElement, std::string_view>> table{
        {IconElement::Disk, "Disk"}, {IconElement::LiveInput, "Live input"}, {IconElement::Audio, "Audio or sample"},
        {IconElement::Oscillator, "Oscillator"}, {IconElement::Spectrum, "Spectrum"}, {IconElement::Input, "Input"},
        {IconElement::Midi, "MIDI"}, {IconElement::Arithmetic, "Arithmetic"}, {IconElement::Frequency, "freq"},
        {IconElement::Time, "time"}, {IconElement::Amplitude, "amp"}, {IconElement::Text, "Text"},
        {IconElement::Pitch, "Pitch"}, {IconElement::More, "Etc... more of the same"},
        {IconElement::ControlPoint, "Control point"}, {IconElement::Blocking, "Blocking"},
        {IconElement::Scale, "Scale, modify"}, {IconElement::SideEffect, "Non-audio side-effect"},
        {IconElement::Process, "Process"}, {IconElement::Action, "Action"}, {IconElement::Control, "Control"},
        {IconElement::Write, "Write"}, {IconElement::Read, "Read"}};
    for (const auto& [element, meaning] : table)
        require(talas::app::iconElementMeaning(element) == meaning, "an element means " + std::string{meaning});
    require(talas::app::iconColourName(IconColour::Amplitude) == "blue green" && talas::app::iconColourName(IconColour::Frequency) == "rose"
            && talas::app::iconColourName(IconColour::Time) == "violet",
        "blue green is amplitude, rose frequency, violet time");
    require(glyphs("talas.oscillator") == std::vector<IconGlyph>{{IconElement::Oscillator}}
            && glyphs("talas.midiOutputEvent") == std::vector<IconGlyph>{{IconElement::Midi}, {IconElement::SideEffect}}
            && glyphs("talas.diskPlayer").front().element == IconElement::Disk
            && glyphs("talas.grainCloud").front().element == IconElement::Cloud,
        "an oscillator's circle, MIDI out's connector and upward arrow, a disk player's disk, a grain cloud");
    // The pop quiz's first icon, the FrequencyTracker: audio in, a
    // subaudio envelope out, extracting the frequency of its input.
    require(shapeOf(registry, "talas.frequencyTracker") == SoundIconShape{IconEdge::Audio, IconEdge::Subaudio, false}
            && glyphs("talas.frequencyTracker") == std::vector<IconGlyph>{{IconElement::Audio}, {IconElement::Output, IconColour::Frequency}},
        "a FrequencyTracker takes audio and gives its frequency envelope");
}

// A
// double-click on a Sound opens its parameters, on an empty area closes
// them; on a Sound made of others it opens them, on a connected input it
// takes the line away.
void testDoubleClicksInTheSignalFlow()
{
    using talas::app::SignalFlowDoubleClick;
    const auto click = [](bool onSound, bool madeOfSounds, bool onInput, bool connected, bool editsLines = true) {
        return talas::app::signalFlowDoubleClick({onSound, madeOfSounds, onInput, connected, editsLines});
    };
    require(click(true, false, false, false) == SignalFlowDoubleClick::OpenParameters
            && click(true, false, false, false, false) == SignalFlowDoubleClick::OpenParameters,
        "a double-click on a Sound should open its parameters");
    require(click(false, false, false, false) == SignalFlowDoubleClick::CloseParameters,
        "a double-click on an empty area should close the parameters and draw the diagram again");
    require(click(true, true, false, false) == SignalFlowDoubleClick::OpenItsSounds,
        "a Sound made of other Sounds should open them");
    require(click(true, false, true, true) == SignalFlowDoubleClick::Disconnect
            && click(true, false, true, false) == SignalFlowDoubleClick::Nothing
            && click(true, false, true, true, false) == SignalFlowDoubleClick::OpenParameters,
        "a double-click on a connected input should take its line away where lines are edited");
}

int main()
{
    try
    {
        testDoubleClicksInTheSignalFlow();
        testAudioSourcesAndEnvelopes();
        testSpectra();
        testSideEffectsEnvironmentsAndConstructors();
        testInnerIcons();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
