#pragma once

#include "talas/project/ProjectDocument.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// A Timeline track's audio input and submixes:
// the track input control's menu of the submixes the track may take, each
// submix's colour, shown in the Submix status of the tracks sending to it
// and on the input name of the tracks taking it, choosing an input, and
// the Submix menu connecting and disconnecting the track's sends.
namespace talas::app
{
struct TimelineTrackInputChoice
{
    std::string trackId;
    std::string displayName;
    std::size_t colourIndex = 0;

    bool operator==(const TimelineTrackInputChoice&) const = default;
};

// The submix's place among the Timeline's submixes, which picks its colour.
[[nodiscard]] std::optional<std::size_t> timelineSubmixColourIndex(
    const talas::project::ProjectScore& score, std::string_view submixId);

// Only the allowable sources, so a submix the track's output goes to
// is absent.
[[nodiscard]] std::vector<TimelineTrackInputChoice> timelineTrackInputChoices(
    const talas::project::ProjectScore& score, std::string_view trackId);

// The project with the track taking the submix as its input, or none for
// an empty submix ID. A track with no Sounds yet gets the input itself, a
// Sound from the start of the Timeline as long as the longest track
// feeding the submix.
[[nodiscard]] std::optional<talas::project::ProjectDocument> timelineWithTrackInput(
    const talas::project::ProjectDocument& document,
    std::string_view trackId,
    std::string_view inputTrackId,
    std::string& error);

// The track taking one of the hardware's audio inputs (from 1)
// instead of a submix; and the input source shown within square brackets
// on the left of the track: "--" for none, the submix's name or
// "Input: n".
[[nodiscard]] std::optional<talas::project::ProjectDocument> timelineWithTrackHardwareInput(
    const talas::project::ProjectDocument& document,
    std::string_view trackId,
    std::uint32_t channel,
    std::string& error);
[[nodiscard]] std::string timelineTrackInputLabel(const talas::project::ProjectScore& score, std::string_view trackId);

// The Submix menu's top level, each existing submix the track may
// go to and whether it is connected; the one its output goes to stays.
struct TimelineSubmixMenuItem
{
    std::string trackId;
    std::string displayName;
    std::size_t colourIndex = 0;
    bool connected = false;
    bool output = false;

    bool operator==(const TimelineSubmixMenuItem&) const = default;
};

[[nodiscard]] std::vector<TimelineSubmixMenuItem> timelineSubmixMenu(
    const talas::project::ProjectScore& score, std::string_view trackId);

// The Submix status, the colours of the submixes the track goes to,
// its output's first; only 16 show.
[[nodiscard]] std::vector<std::size_t> timelineSubmixStatus(
    const talas::project::ProjectScore& score, std::string_view trackId);

// Connecting the track to a submix or disconnecting it.
[[nodiscard]] std::optional<talas::project::ProjectDocument> timelineWithTrackSend(
    const talas::project::ProjectDocument& document,
    std::string_view trackId,
    std::string_view submixId,
    bool connected,
    std::string& error);

// The audio output menu: the track hard-wired to output
// channels (from 1), or back to its pan controls with none; and the Output
// channel status, a light for each of the Master's channels, lit for those
// the track is wired to, none while it uses its pan controls.
[[nodiscard]] std::optional<talas::project::ProjectDocument> timelineWithTrackOutputs(
    const talas::project::ProjectDocument& document,
    std::string_view trackId,
    std::vector<std::uint32_t> channels,
    std::string& error);
[[nodiscard]] std::vector<bool> timelineOutputStatus(
    const talas::project::ProjectScore& score, std::string_view trackId);
// No output, the track heard only through the submixes it sends to;
// its Output channel status shows every light off.
[[nodiscard]] std::optional<talas::project::ProjectDocument> timelineWithTrackNoOutput(
    const talas::project::ProjectDocument& document,
    std::string_view trackId,
    std::string& error);

// The track's MIDI input channel, from 1 to 16, on which the
// Sounds in it expect MIDI; 0 for the project's default channel.
[[nodiscard]] std::optional<talas::project::ProjectDocument> timelineWithTrackMidiChannel(
    const talas::project::ProjectDocument& document,
    std::string_view trackId,
    std::uint8_t channel,
    std::string& error);

// The track input's status indicators for the Sound under the time
// cursor: the microphone green when it expects audio input (an audio input
// of its own, which a track input replaces), the MIDI indicator green when
// it expects MIDI.
struct TimelineInputStatus
{
    bool audio = false;
    bool midi = false;
    // Red while its parameters are being recorded.
    bool recording = false;

    bool operator==(const TimelineInputStatus&) const = default;
};

[[nodiscard]] TimelineInputStatus timelineClipInputStatus(
    const talas::project::ProjectDocument& document, std::string_view clipId);

// The random letter supplied as a new submix's name, one no
// submix has yet; and a new submix of that name (or the one typed)
// connected to the track.
[[nodiscard]] std::string timelineNewSubmixName(const talas::project::ProjectScore& score, std::uint64_t seed);
[[nodiscard]] std::optional<talas::project::ProjectDocument> timelineWithNewSubmix(
    const talas::project::ProjectDocument& document,
    std::string_view trackId,
    std::string_view name,
    std::string& error);
}
