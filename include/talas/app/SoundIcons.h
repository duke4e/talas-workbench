#pragma once

#include "talas/core/NodeRegistry.h"

#include <cstdint>
#include <string_view>
#include <vector>

// A Sound icon's outer shape: the left edge
// tells the kind of input the Sound takes, the right edge the kind of
// output it gives, and a pointed top a side effect outside the signal flow
// (a display, MIDI out, a recording).
namespace talas::app
{
enum class IconEdge
{
    // Straight: no input, or no output.
    None,
    // Rounded: audio.
    Audio,
    // Rounded and small: a subaudio control signal, as an envelope gives.
    Subaudio,
    // Rounded, small and double: whatever came in goes out.
    Transparent,
    // Triangular: a spectrum, amplitude and frequency envelopes a partial.
    Spectrum,
    // Triangular, small and double: algorithmically made, so unknown.
    Variable,
    // Triangular, pointing left: controls supplied to the input Sound.
    Environment,
    // Triangular, small, pointing left: Sounds to the left made into a new
    // one.
    Constructor,
};

struct SoundIconShape
{
    IconEdge left = IconEdge::None;
    IconEdge right = IconEdge::None;
    bool pointedTop = false;

    bool operator==(const SoundIconShape&) const = default;
};

// The shape of a kind of Sound, from its inputs, outputs and category:
// a control parameter's port is no input of the Sound's; a Script or file
// interpreter constructs, a sequencer or MIDI voice with an input is an
// environment, and a variable stands for a Sound not yet known.
[[nodiscard]] SoundIconShape soundIconShape(const talas::core::NodeTypeDescriptor& descriptor);
[[nodiscard]] std::string_view iconEdgeName(IconEdge edge) noexcept;

// The inner icon's sub-elements: nouns, and the verbs and
// combinations that act on them.
enum class IconElement
{
    File,
    Disk,
    Ram,
    LiveInput,
    Audio,
    Oscillator,
    Spectrum,
    Cloud,
    Input,
    Midi,
    Output,
    Arithmetic,
    Frequency,
    Time,
    Amplitude,
    Text,
    Pitch,
    More,
    ControlPoint,
    // Layering two elements: more than one.
    Several,
    Blocking,
    Enabling,
    Scale,
    Cycle,
    SideEffect,
    Process,
    Action,
    Control,
    Write,
    Read,
    Formants,
    // A bandpass filter: three inputs, the outer two blocked, the middle
    // one enabled and turned into the output.
    Bandpass,
};

// Colour tells which of amplitude, frequency or time an element is about.
enum class IconColour
{
    Plain,
    Amplitude,
    Frequency,
    Time,
};

struct IconGlyph
{
    IconElement element = IconElement::Audio;
    IconColour colour = IconColour::Plain;
    bool operator==(const IconGlyph&) const = default;
};

// The inner icon of a kind of Sound, its elements left to right.
[[nodiscard]] std::vector<IconGlyph> soundIconGlyphs(const talas::core::NodeTypeDescriptor& descriptor);
// What an element or colour means.
[[nodiscard]] std::string_view iconElementMeaning(IconElement element) noexcept;
[[nodiscard]] std::string_view iconColourName(IconColour colour) noexcept;

// A double-click in the signal flow: on a Sound made of other Sounds it opens them;
// on a connected input it takes the line away; on any other Sound it opens
// that Sound's parameters below, in place of the ones open, for one
// Sound's are open at a time, its icon patterned while they are; on an
// empty area it closes them and draws the diagram again from the Sounds as
// they are.
enum class SignalFlowDoubleClick : std::uint8_t
{
    OpenParameters,
    OpenItsSounds,
    Disconnect,
    CloseParameters,
    Nothing,
};

struct SignalFlowClickTarget
{
    bool onSound = false;
    bool madeOfSounds = false;
    bool onInput = false;
    bool inputConnected = false;
    // A view that edits lines; a read-only one leaves its inputs alone.
    bool editsLines = true;
};

[[nodiscard]] SignalFlowDoubleClick signalFlowDoubleClick(const SignalFlowClickTarget& target) noexcept;
}
