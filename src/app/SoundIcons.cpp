#include "talas/app/SoundIcons.h"

#include <algorithm>
#include <initializer_list>
#include <string>

namespace talas::app
{
namespace
{
using talas::core::PortType;

bool isSpectrum(PortType type) noexcept
{
    return type == PortType::Spectrum || type == PortType::ComplexSpectrum;
}

bool anyPort(const std::vector<talas::core::PortDescriptor>& ports, bool (*test)(PortType) noexcept)
{
    return std::ranges::any_of(ports, [&](const talas::core::PortDescriptor& port) { return test(port.type); });
}

// An input the Sound processes, as against a control or an optional
// modulation of one of its parameters (an Oscillator's modulator).
bool processesInput(const talas::core::PortDescriptor& port)
{
    if (isSpectrum(port.type))
        return true;
    return (port.type == PortType::Audio || port.type == PortType::SampleBuffer)
        && (port.required || port.id == "audioIn" || port.id == "inputs");
}
}

SoundIconShape soundIconShape(const talas::core::NodeTypeDescriptor& descriptor)
{
    const auto& category = descriptor.category;
    const auto hasInput = std::ranges::any_of(descriptor.inputs, processesInput);
    SoundIconShape shape;
    if (category == "Scripts")
        return {IconEdge::Constructor, IconEdge::Variable, false};
    if (category == "Variables" || category == "Variables & Annotation")
        return {IconEdge::None, IconEdge::Variable, false};
    if ((category == "Sequencers" || category == "MIDI In") && hasInput)
        return {IconEdge::Environment, IconEdge::Transparent, false};

    if (anyPort(descriptor.inputs, isSpectrum))
        shape.left = IconEdge::Spectrum;
    else if (hasInput)
        shape.left = IconEdge::Audio;

    if (anyPort(descriptor.outputs, isSpectrum))
        shape.right = IconEdge::Spectrum;
    else if (std::ranges::any_of(descriptor.outputs, [](const auto& port) { return port.type == PortType::Audio; }))
        shape.right = IconEdge::Audio;
    else if (!descriptor.outputs.empty())
        shape.right = IconEdge::Subaudio;
    if (category == "Envelopes & Control Signals" && !hasInput)
        shape.right = IconEdge::Subaudio;

    // A display passes its input on as it was; it and MIDI out and a
    // recording have side effects.
    if (category == "Display" || category == "Annotation")
        shape.right = IconEdge::Transparent;
    // A MemoryWriter passes its input on as it stores it.
    if (descriptor.typeId == "talas.memoryWriter")
        shape.right = IconEdge::Transparent;
    shape.pointedTop = category == "Display" || category == "MIDI Out" || descriptor.typeId == "talas.diskRecorder"
        || descriptor.typeId == "talas.memoryWriter";
    return shape;
}

std::vector<IconGlyph> soundIconGlyphs(const talas::core::NodeTypeDescriptor& descriptor)
{
    using E = IconElement;
    using C = IconColour;
    const auto& id = descriptor.typeId;
    const auto& category = descriptor.category;
    const auto has = [&](std::string_view part) { return id.find(part) != std::string::npos; };
    const auto plain = [](std::initializer_list<E> elements) {
        std::vector<IconGlyph> glyphs;
        for (const auto element : elements)
            glyphs.push_back({element, C::Plain});
        return glyphs;
    };
    // Particular Sounds' icons.
    if (id == "talas.sumOfSines")
        return plain({E::Oscillator, E::Several, E::File, E::Spectrum});
    if (id == "talas.filterBank")
        return {{E::Bandpass, C::Frequency}, {E::Several, C::Plain}};
    if (id == "talas.memoryWriter")
        return plain({E::Ram, E::Write});
    if (id == "talas.amplitudeFollower")
        return {{E::Audio, C::Plain}, {E::Output, C::Amplitude}};
    if (id == "talas.frequencyTracker")
        return {{E::Audio, C::Plain}, {E::Output, C::Frequency}};
    if (id == "talas.diskRecorder")
        return plain({E::Disk, E::Write});
    if (has("formant") || has("Formant"))
        return has("Bank") ? plain({E::Formants, E::Several}) : plain({E::Formants});
    if (has("loud") || has("grain") || has("Grain"))
        return has("Bank") ? plain({E::Cloud, E::Several}) : plain({E::Cloud});
    if (id == "talas.oscillatorBank")
        return plain({E::Oscillator, E::Several, E::Spectrum});
    if (id == "talas.spectrumInRam")
        return plain({E::Spectrum, E::Ram});
    if (id == "talas.spectrumOnDisk")
        return plain({E::Spectrum, E::Disk});
    if (id == "talas.liveSpectralAnalysis")
        return plain({E::Audio, E::Process, E::Spectrum});
    if (has("disk") || has("Disk"))
        return plain({E::Disk, E::Audio});
    if (category == "Sampling")
        return has("ulti") ? plain({E::Ram, E::Audio, E::Several}) : plain({E::Ram, E::Audio});
    if (has("scillator"))
        return plain({E::Oscillator});
    if (category == "Spectral Modifiers" || category == "Spectral Utilities" || category == "Spectral"
        || category == "Spectral Sources" || category == "Spectral Analysis FFT")
        return plain({E::Spectrum, E::Process});
    if (category == "Filters")
        return {{E::Bandpass, C::Frequency}};
    if (category == "Frequency & Time Scaling")
        return {{E::Scale, C::Frequency}, {E::Scale, C::Time}};
    if (category == "Level, Compression, Expansion")
        return {{E::Scale, C::Amplitude}};
    if (category == "Reverb, Delay, Feedback" || category == "Effects")
        return {{E::Cycle, C::Time}};
    if (category == "Envelopes & Control Signals")
        return {{E::ControlPoint, C::Amplitude}};
    if (category == "Control" || category == "Control Utilities")
        return plain({E::Control});
    if (category == "Math")
        return plain({E::Arithmetic});
    if (category == "MIDI In")
        return plain({E::Midi, E::Input});
    if (category == "MIDI Out")
        return plain({E::Midi, E::SideEffect});
    if (id == "talas.textFileInterpreter")
        return plain({E::File, E::Text});
    if (id == "talas.midiFileInterpreter")
        return plain({E::File, E::Midi});
    if (category == "Scripts")
        return plain({E::Text, E::Action});
    if (category == "Sequencers")
        return plain({E::Cycle, E::ControlPoint});
    if (category == "Display")
        return plain({E::Read});
    if (category == "Annotation" || category == "Variables & Annotation")
        return plain({E::Text});
    if (category == "Variables")
        return plain({E::More});
    if (category == "Mixing" || category == "Mixing & Panning")
        return plain({E::Audio, E::Several});
    if (category == "Tracking Live Input")
        return plain({E::LiveInput, E::Process});
    if (category == "IO")
        return plain({E::Output});
    if (category == "Sources" || category == "Xtra Sources" || category == "Generators" || category == "Oscillators")
        return plain({E::Audio});
    return plain({E::Audio, E::Process});
}

std::string_view iconElementMeaning(IconElement element) noexcept
{
    switch (element)
    {
    case IconElement::File: return "File";
    case IconElement::Disk: return "Disk";
    case IconElement::Ram: return "RAM";
    case IconElement::LiveInput: return "Live input";
    case IconElement::Audio: return "Audio or sample";
    case IconElement::Oscillator: return "Oscillator";
    case IconElement::Spectrum: return "Spectrum";
    case IconElement::Cloud: return "Grain cloud";
    case IconElement::Input: return "Input";
    case IconElement::Midi: return "MIDI";
    case IconElement::Output: return "Output or result";
    case IconElement::Arithmetic: return "Arithmetic";
    case IconElement::Frequency: return "freq";
    case IconElement::Time: return "time";
    case IconElement::Amplitude: return "amp";
    case IconElement::Text: return "Text";
    case IconElement::Pitch: return "Pitch";
    case IconElement::More: return "Etc... more of the same";
    case IconElement::ControlPoint: return "Control point";
    case IconElement::Several: return "More than one";
    case IconElement::Blocking: return "Blocking";
    case IconElement::Enabling: return "Transparent, enabling";
    case IconElement::Scale: return "Scale, modify";
    case IconElement::Cycle: return "Cycle, sequence";
    case IconElement::SideEffect: return "Non-audio side-effect";
    case IconElement::Process: return "Process";
    case IconElement::Action: return "Action";
    case IconElement::Control: return "Control";
    case IconElement::Write: return "Write";
    case IconElement::Read: return "Read";
    case IconElement::Formants: return "Process formants";
    case IconElement::Bandpass: return "Bandpass filter";
    }
    return {};
}

std::string_view iconColourName(IconColour colour) noexcept
{
    switch (colour)
    {
    case IconColour::Plain: return "plain";
    case IconColour::Amplitude: return "blue green";
    case IconColour::Frequency: return "rose";
    case IconColour::Time: return "violet";
    }
    return {};
}

std::string_view iconEdgeName(IconEdge edge) noexcept
{
    switch (edge)
    {
    case IconEdge::None: return "straight";
    case IconEdge::Audio: return "rounded";
    case IconEdge::Subaudio: return "rounded, small";
    case IconEdge::Transparent: return "rounded, small, double";
    case IconEdge::Spectrum: return "triangular";
    case IconEdge::Variable: return "triangular, small, double";
    case IconEdge::Environment: return "triangular, pointing left";
    case IconEdge::Constructor: return "triangular, small, pointing left";
    }
    return {};
}

SignalFlowDoubleClick signalFlowDoubleClick(const SignalFlowClickTarget& target) noexcept
{
    if (target.onSound && target.madeOfSounds)
        return SignalFlowDoubleClick::OpenItsSounds;
    if (target.editsLines && target.onInput)
        return target.inputConnected ? SignalFlowDoubleClick::Disconnect : SignalFlowDoubleClick::Nothing;
    return target.onSound ? SignalFlowDoubleClick::OpenParameters : SignalFlowDoubleClick::CloseParameters;
}
}
