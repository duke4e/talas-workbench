#pragma once

#include "talas/app/AnalysisToolsModel.h"
#include "talas/project/ProjectDocument.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace talas::app
{
// A Standard MIDI File's length in seconds: its last event's time, by its
// tempo changes (120 beats a minute until the first), its division in
// ticks a quarter note or SMPTE frames; nothing for bytes that are not a
// Standard MIDI File.
[[nodiscard]] std::optional<double> standardMidiFileSeconds(std::span<const std::uint8_t> bytes);

// The channels, 1 to 16, whose notes a Standard MIDI File starts, in order;
// none for bytes that are not one.
[[nodiscard]] std::vector<std::uint8_t> standardMidiFileChannels(std::span<const std::uint8_t> bytes);

// Importing a MIDI file into the Timeline: a Sound of a MIDIVoice that
// plays the file (Source "MIDI File", up to eight notes at once) on an
// oscillator at its keys' pitches and velocities, its midiFile field the
// file, for a clip as long as the file on a new track.
[[nodiscard]] AnalysisExampleSound midiFileTimelineSound(
    const talas::project::ProjectDocument& document,
    const std::filesystem::path& midiFile);

// A MIDI file dragged into the Sound list: a Mixer
// of MIDIVoices, one for each channel of the file, each with the minimal
// instrument above to orchestrate; one channel, one MIDIVoice.
[[nodiscard]] AnalysisExampleSound midiFileOrchestraSound(
    const talas::project::ProjectDocument& document,
    const std::filesystem::path& midiFile,
    std::span<const std::uint8_t> channels);

// The Timeline edits that place it: a new track named for the file (and a
// Master when the Timeline has none) and a clip of the Sound from time 0
// for the file's length.
[[nodiscard]] talas::project::ProjectScoreEditBatch midiFileTimelineEdits(
    const talas::project::ProjectDocument& document,
    std::string_view definitionId,
    std::string_view displayName,
    double seconds);
}
