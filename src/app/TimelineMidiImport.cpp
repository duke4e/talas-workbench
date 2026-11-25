#include "talas/app/TimelineMidiImport.h"

#include <algorithm>
#include <map>
#include <vector>

namespace talas::app
{
namespace
{
[[nodiscard]] std::uint32_t big(std::span<const std::uint8_t> bytes, std::size_t at, std::size_t count)
{
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < count; ++index)
        value = value << 8 | bytes[at + index];
    return value;
}

// A variable-length quantity, moving `at` past it.
[[nodiscard]] std::optional<std::uint32_t> variableLength(std::span<const std::uint8_t> bytes, std::size_t& at, std::size_t end)
{
    std::uint32_t value = 0;
    for (int count = 0; count < 4; ++count)
    {
        if (at >= end)
            return std::nullopt;
        const auto byte = bytes[at++];
        value = value << 7 | (byte & 0x7Fu);
        if ((byte & 0x80u) == 0)
            return value;
    }
    return std::nullopt;
}
}

namespace
{
struct MidiFileWalk
{
    std::uint32_t division = 0;
    std::uint64_t lastTick = 0;
    // Tempo changes by tick, in microseconds a quarter note.
    std::map<std::uint64_t, std::uint32_t> tempi;
    // The channels, 1 to 16, whose notes start.
    std::vector<std::uint8_t> noteChannels;
};

[[nodiscard]] std::optional<MidiFileWalk> walkStandardMidiFile(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < 14 || big(bytes, 0, 4) != 0x4D546864u || big(bytes, 4, 4) < 6)
        return std::nullopt;
    MidiFileWalk walk;
    const auto headerLength = big(bytes, 4, 4);
    const auto tracks = big(bytes, 10, 2);
    walk.division = big(bytes, 12, 2);
    if (walk.division == 0)
        return std::nullopt;
    auto at = std::size_t{8} + headerLength;
    for (std::uint32_t track = 0; track < tracks; ++track)
    {
        if (at + 8 > bytes.size() || big(bytes, at, 4) != 0x4D54726Bu)
            return std::nullopt;
        const auto length = big(bytes, at + 4, 4);
        at += 8;
        const auto end = at + length;
        if (end > bytes.size())
            return std::nullopt;
        std::uint64_t tick = 0;
        std::uint8_t running = 0;
        while (at < end)
        {
            const auto delta = variableLength(bytes, at, end);
            if (!delta || at >= end)
                return std::nullopt;
            tick += *delta;
            auto status = bytes[at];
            if (status == 0xFF)
            {
                if (at + 2 > end)
                    return std::nullopt;
                const auto type = bytes[at + 1];
                at += 2;
                const auto size = variableLength(bytes, at, end);
                if (!size || at + *size > end)
                    return std::nullopt;
                if (type == 0x51 && *size == 3)
                    walk.tempi[tick] = big(bytes, at, 3);
                at += *size;
            }
            else if (status == 0xF0 || status == 0xF7)
            {
                ++at;
                const auto size = variableLength(bytes, at, end);
                if (!size || at + *size > end)
                    return std::nullopt;
                at += *size;
            }
            else
            {
                if ((status & 0x80u) != 0)
                {
                    running = status;
                    ++at;
                }
                else if (running == 0)
                    return std::nullopt;
                const auto kind = running & 0xF0u;
                const auto data = kind == 0xC0u || kind == 0xD0u ? 1u : 2u;
                if (at + data > end)
                    return std::nullopt;
                if (kind == 0x90u && bytes[at + 1] > 0)
                {
                    const auto channel = static_cast<std::uint8_t>((running & 0x0Fu) + 1);
                    if (std::ranges::find(walk.noteChannels, channel) == walk.noteChannels.end())
                        walk.noteChannels.push_back(channel);
                }
                at += data;
            }
            walk.lastTick = std::max(walk.lastTick, tick);
        }
        at = end;
    }
    std::ranges::sort(walk.noteChannels);
    return walk;
}
}

std::optional<double> standardMidiFileSeconds(std::span<const std::uint8_t> bytes)
{
    const auto walk = walkStandardMidiFile(bytes);
    if (!walk.has_value())
        return std::nullopt;
    const auto division = walk->division;
    const auto lastTick = walk->lastTick;
    if ((division & 0x8000u) != 0)
    {
        // SMPTE: frames a second times ticks a frame.
        const auto frames = static_cast<double>(256 - (division >> 8));
        const auto perFrame = static_cast<double>(division & 0xFFu);
        return frames > 0.0 && perFrame > 0.0 ? std::optional{static_cast<double>(lastTick) / (frames * perFrame)} : std::nullopt;
    }
    auto seconds = 0.0;
    std::uint64_t tick = 0;
    std::uint32_t tempo = 500000;
    for (const auto& [changeTick, changeTempo] : walk->tempi)
    {
        if (changeTick >= lastTick)
            break;
        seconds += static_cast<double>(changeTick - tick) * tempo / 1.0e6 / division;
        tick = changeTick;
        tempo = changeTempo;
    }
    seconds += static_cast<double>(lastTick - tick) * tempo / 1.0e6 / division;
    return seconds;
}

std::vector<std::uint8_t> standardMidiFileChannels(std::span<const std::uint8_t> bytes)
{
    const auto walk = walkStandardMidiFile(bytes);
    return walk.has_value() ? walk->noteChannels : std::vector<std::uint8_t>{};
}

talas::project::ProjectScoreEditBatch midiFileTimelineEdits(
    const talas::project::ProjectDocument& document,
    std::string_view definitionId,
    std::string_view displayName,
    double seconds)
{
    const auto& score = document.timeline.score;
    const auto unique = [&](std::string base) {
        for (std::size_t ordinal = 1;; ++ordinal)
        {
            auto id = base + std::to_string(ordinal);
            const auto used = std::ranges::any_of(score.tracks, [&](const auto& track) { return track.trackId == id; })
                || std::ranges::any_of(score.clips, [&](const auto& clip) { return clip.clipId == id; });
            if (!used)
                return id;
        }
    };
    talas::project::ProjectScoreEditBatch edits;
    std::string masterId;
    const auto master = std::ranges::find(score.tracks, talas::project::ScoreTrackKind::Master, &talas::project::ScoreTrack::kind);
    if (master == score.tracks.end())
    {
        masterId = unique("master");
        edits.tracks.push_back({masterId, "Master", talas::project::ScoreTrackKind::Master,
            talas::project::ScoreTimeDomain::Seconds, {}, 2, talas::project::ScoreOverlapMode::Layer});
    }
    else
    {
        masterId = master->trackId;
    }
    const auto trackId = unique("midiTrack");
    edits.tracks.push_back({trackId, std::string{displayName}, talas::project::ScoreTrackKind::Track,
        talas::project::ScoreTimeDomain::Seconds, masterId, 2, talas::project::ScoreOverlapMode::Layer});
    const talas::project::ScoreTime zero{talas::project::ScoreTimeDomain::Seconds, 0.0, 0};
    edits.clips.push_back({unique("midiClip"), trackId, std::string{definitionId}, zero,
        {talas::project::ScoreTimeDomain::Seconds, std::max(seconds, 0.001), 0}, zero, zero, zero, 1});
    return edits;
}

AnalysisExampleSound midiFileTimelineSound(
    const talas::project::ProjectDocument& document,
    const std::filesystem::path& midiFile)
{
    AnalysisExampleSound example;
    auto& definition = example.definition;
    auto stem = midiFile.stem().string();
    std::string base = "midiFile";
    for (std::size_t ordinal = 1;; ++ordinal)
    {
        definition.definitionId = base + std::to_string(ordinal);
        if (std::ranges::none_of(document.soundDefinitions, [&](const auto& existing) {
                return existing.definitionId == definition.definitionId;
            }))
            break;
    }
    definition.displayName = stem.empty() ? std::string{"MIDI file"} : stem;
    const auto expression = [](std::string id, std::string value) {
        talas::project::ProjectParameterExpression parameter;
        parameter.parameterId = std::move(id);
        parameter.expression = std::move(value);
        return parameter;
    };
    talas::project::ProjectParameterExpression source;
    source.parameterId = "source";
    source.text = "MIDI File";
    definition.graph.nodes.push_back({"key", "talas.oscillator",
        {expression("frequency", "!KeyPitch"), expression("amplitude", "(!KeyDown > 0) * !KeyVelocity * 0.25")}});
    definition.graph.nodes.push_back({"voice", "talas.midiVoice", {source, expression("polyphony", "8")}});
    definition.graph.connections.push_back({"key", "audioOut", "voice", "audioIn"});
    definition.outputs.push_back({"audioOut", talas::core::PortType::Audio, "voice", "leftOut"});
    example.resources.push_back({"voice", "midiFile", midiFile});
    return example;
}

AnalysisExampleSound midiFileOrchestraSound(
    const talas::project::ProjectDocument& document,
    const std::filesystem::path& midiFile,
    std::span<const std::uint8_t> channels)
{
    auto example = midiFileTimelineSound(document, midiFile);
    if (channels.size() < 2)
        return example;
    auto& definition = example.definition;
    const auto voice = definition.graph.nodes[1];
    const auto key = definition.graph.nodes[0];
    definition.graph = {};
    example.resources.clear();
    definition.graph.nodes.push_back({"mix", "talas.mixerV2", {}});
    for (const auto channel : channels)
    {
        const auto suffix = std::to_string(channel);
        auto instrument = key;
        instrument.instanceId = "key" + suffix;
        auto player = voice;
        player.instanceId = "voice" + suffix;
        talas::project::ProjectParameterExpression channelParameter;
        channelParameter.parameterId = "channel";
        channelParameter.expression = suffix;
        player.parameters.push_back(std::move(channelParameter));
        definition.graph.nodes.push_back(std::move(instrument));
        definition.graph.nodes.push_back(std::move(player));
        definition.graph.connections.push_back({"key" + suffix, "audioOut", "voice" + suffix, "audioIn"});
        definition.graph.connections.push_back({"voice" + suffix, "leftOut", "mix", "inputs"});
        example.resources.push_back({"voice" + suffix, "midiFile", midiFile});
    }
    definition.outputs = {{"audioOut", talas::core::PortType::Audio, "mix", "leftOut"}};
    return example;
}
}
