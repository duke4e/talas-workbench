#pragma once

#include "talas/expr/Expression.h"
#include "talas/plugin/PluginIdentity.h"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace talas::core
{
class NodeRegistry;
}

namespace talas::project
{
struct ProjectDocument;
}

// ADR-0029: which of a project's Event Values an exported plugin shows its
// host as parameters, and the plugin state that saves them. JUCE-free.
namespace talas::plugin
{
enum class PluginParameterKind
{
    // A fader: a continuous range.
    Continuous,
    // A toggle: off (minimum) or on (maximum).
    Toggle,
    // A gate: on while held, off otherwise.
    Momentary,
};

// More would crowd every host's generic view; the rest stay at their defaults.
inline constexpr std::size_t maximumPluginParameters = 128;

struct PluginParameterSpec
{
    // The Event Value, without its '!'. It is also the parameter's stable ID.
    std::string eventName;
    // What the host shows: the control's label without a trailing
    // "(unit)", or the Event Value's name.
    std::string displayName;
    // The unit the host shows after the value: the label's "(unit)" or the
    // domain's (Hz, s, nn, samples).
    std::string unit;
    talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;
    PluginParameterKind kind = PluginParameterKind::Continuous;
    double minimum = 0.0;
    double maximum = 1.0;
    double defaultValue = 0.0;
    // A log-tapered fader (minimum above zero) moves through its range
    // geometrically.
    bool logarithmic = false;
    // The control's grid, or zero for none.
    double step = 0.0;
    // True for a control the project declares; false for one inferred from
    // an Event Value the graph reads without one.
    bool declared = true;

    bool operator==(const PluginParameterSpec&) const = default;
};

struct PluginParameterMapping
{
    std::vector<PluginParameterSpec> parameters;
    std::vector<std::string> warnings;
};

// The project's Virtual Controls in their order, then the Event Values its
// graph reads without one (with the range their name or the global map
// gives them). An instrument leaves out the MIDI-driven names (MIDIPitchBend,
// the controllers the default MIDI map names) its host sets through MIDI.
[[nodiscard]] PluginParameterMapping mapProjectParameters(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    PluginKind kind);

// The parameter's value at a normalized position in [0, 1], and back; a
// toggle or gate snaps to its ends, a grid to its steps.
[[nodiscard]] double parameterValueFromNormalized(const PluginParameterSpec& spec, double normalized) noexcept;
[[nodiscard]] double parameterValueToNormalized(const PluginParameterSpec& spec, double value) noexcept;
[[nodiscard]] double clampParameterValue(const PluginParameterSpec& spec, double value) noexcept;

// The live audio input channels (one-based 1 to 8) the project's Audio Input
// Sounds read, anywhere in the project.
[[nodiscard]] std::array<bool, 8> projectAudioInputChannels(const talas::project::ProjectDocument& document);
// Whether some MIDI Voice in the project plays live MIDI (its source
// "Live MIDI"), so a host's notes reach it.
[[nodiscard]] bool projectPlaysLiveMidi(const talas::project::ProjectDocument& document);

inline constexpr std::string_view pluginStateFormat = "talas.plugin-state";
inline constexpr int pluginStateFormatVersion = 1;

struct PluginState
{
    std::string pluginName;
    std::string projectHash;
    std::vector<std::pair<std::string, double>> parameters;

    bool operator==(const PluginState&) const = default;
};

[[nodiscard]] std::string serializePluginState(const PluginState& state);
[[nodiscard]] std::optional<PluginState> parsePluginState(std::string_view json, std::string* error = nullptr);
}
