#pragma once

#include "talas/project/ProjectDocument.h"
#include "talas/project/ProjectMidiMapperMap.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The global map: the associations of Event Values with MIDI controllers and
// the pitch wheel that every Sound hears without a map of its own, one
// association a line:
//   !Volume is: (`MIDIController07 channel: 5; min: 0.5 max: 0.75 grid: 0.05; taper: #log).
//   !DamperPedal is: (`MIDIController64 displayAs: #gate).
//   !PitchBend is: `MIDIPitchBend.
// A project without one of its own uses the Default map (ADR-0024).
namespace talas::project
{
inline constexpr std::size_t maximumGlobalMapBytes = 16384;
inline constexpr std::size_t maximumGlobalMapEntries = 128;

struct GlobalMapEntry
{
    std::string eventName;
    // The pitch wheel, from -1 to 1, rather than a controller.
    bool pitchBend = false;
    std::uint8_t controller = 0;
    // The MIDI channel; the default channel without one.
    std::optional<std::uint8_t> channel;
    double minimum = 0.0;
    double maximum = 1.0;
    double grid = 0.0;
    MidiMapperMapTaper taper = MidiMapperMapTaper::Linear;
    MidiMapperMapDisplayAs displayAs = MidiMapperMapDisplayAs::Default;
    // Another Event Source, named as any legal variable name, `Glide:
    // no MIDI reaches it, only the Virtual Control Surface and tools set it
    //. Empty for a MIDI
    // controller or the pitch wheel.
    std::string source;

    bool operator==(const GlobalMapEntry&) const = default;
};

struct GlobalMapParseResult
{
    std::vector<GlobalMapEntry> entries;
    std::string error;
    // The line (from 1) the error is on.
    std::size_t line = 0;

    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

[[nodiscard]] GlobalMapParseResult parseProjectGlobalMap(std::string_view text);

// ADR-0024's Default map written in the global map's syntax.
[[nodiscard]] std::string defaultGlobalMapText();

// The associations the project's Sounds hear: its own global map, else the
// Default map, without the names a Virtual Control maps to MIDI or OSC
// itself (ADR-0024). Nothing for a map that does not parse.
[[nodiscard]] std::vector<GlobalMapEntry> projectGlobalMapEntries(const ProjectDocument& document);

// A controller's value from 0 to 1 (or the wheel's from -1 to 1) as the
// entry's Event Value: within its range by its taper, on its grid.
[[nodiscard]] std::optional<double> globalMapValue(const GlobalMapEntry& entry, double normalized) noexcept;

// The kind a Virtual Control made for the entry shows.
[[nodiscard]] VirtualControlKind globalMapControlKind(const GlobalMapEntry& entry) noexcept;

// The Event Source the entry names, as a
// Val field writes it, `MIDIController07, `MIDIPitchBend or `Glide.
// Nothing for a controller on a channel of its own, which a field's source
// does not name.
[[nodiscard]] std::optional<std::string> globalMapEventSource(const GlobalMapEntry& entry);

// The Val expression of the entry's Event Value read from its Event
// Source: the source's value from 0 to 1 within the range by the taper, on
// the grid, as globalMapValue gives it (the wheel's from -1 to 1 as it is).
[[nodiscard]] std::optional<std::string> globalMapSourceExpression(const GlobalMapEntry& entry);

// The value of the entry's Event Source that gives the Event Value `value`
// (globalMapValue's inverse), within the range.
[[nodiscard]] std::optional<double> globalMapSourceValue(const GlobalMapEntry& entry, double value) noexcept;

// An Event Value of a field written as its Event Source's expression.
struct GlobalMapEventSourceRewrite
{
    std::string eventName;
    // The Event Value the source reads (cc07, MIDIPitchBend, Glide).
    std::string sourceEventName;
    GlobalMapEntry entry;

    bool operator==(const GlobalMapEventSourceRewrite&) const = default;
};

// After Expand the fields show blue Event Sources
// where they had the red Event Values the global map names, the map's
// names being mnemonics for its sources. Each Event Value of the graph's
// Val fields that the project's map (else the Default map) holds is
// written as globalMapSourceExpression, so the Sound plays as before: !Volume
// becomes (`MIDIController07). Names the map does not hold (the Virtual
// Control Surface's alone, or mapped by a Virtual Control itself), names in
// `kept` (a Sound's own Event Values), names a MIDIMapper of the graph maps,
// a controller on a channel of its own and an Event Value whose Virtual
// Control is not a scalar stay as they are; nothing for a project that is
// not Val. Only the nodes in `nodeIds` when given. The rewrites made,
// one for each Event Value.
[[nodiscard]] std::vector<GlobalMapEventSourceRewrite> writeGlobalMapEventSources(const ProjectDocument& document,
    ProjectGraph& graph, std::span<const std::string> kept = {}, const std::vector<std::string>* nodeIds = nullptr);

// Escape in a
// parameter field, then MIDI, types the Event Value the MIDI names. Keys
// played together name the keyboard's: one key !KeyPitch, two !KeyDown,
// three !KeyVelocity, or one key !Pitch when the preferences ask for it; named
// once the last key is let go. A continuous controller names the Event Value
// the project's map, else the Default map, gives it on its channel, else its
// own, as !cc07.
class EventValueLearning
{
public:
    // A key going down or up: the name, with its !, once the last is up.
    [[nodiscard]] std::optional<std::string> key(bool down, bool oneKeyGivesPitch = false);
    // The name for a controller moved on a channel, from 1 to 16.
    [[nodiscard]] static std::string controller(
        const ProjectDocument& document, std::uint8_t controller, std::uint8_t channel);

private:
    std::size_t held_ = 0;
    std::size_t most_ = 0;
};
}
