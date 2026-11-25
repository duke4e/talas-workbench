#include "talas/audio/SpectrumTables.h"

#include "talas/audio/AudioFilePublication.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <map>

namespace talas::audio
{
SpectrumInRamTable spectrumInRamTable(
    const talas::project::PartialTrackAsset& spectrum, std::size_t maximumFrames, std::size_t maximumTriples)
{
    SpectrumInRamTable table;
    std::map<std::uint64_t, std::size_t> slots;
    for (const auto& frame : spectrum.frames)
    {
        for (const auto& point : frame.points)
            slots.emplace(point.trackId, 0);
    }
    if (slots.empty() || maximumFrames == 0 || maximumTriples == 0)
        return table;
    table.partials = std::min({slots.size(), maximumTriples, std::size_t{4096}});
    std::size_t slot = 0;
    for (auto it = slots.begin(); it != slots.end(); ++it)
        it->second = slot++;
    table.frames = std::min({maximumFrames, spectrum.frames.size(), maximumTriples / table.partials});
    table.durationSeconds = spectrum.sampleRate > 0.0
        ? static_cast<double>(spectrum.sourceFrameCount) / spectrum.sampleRate
        : 0.0;
    // Each track's frequency in every source frame, carried through the
    // frames it is silent in.
    std::vector<double> lastFrequency(table.partials, 0.0);
    std::vector<double> firstFrequency(table.partials, 0.0);
    for (const auto& frame : spectrum.frames)
    {
        for (const auto& point : frame.points)
        {
            const auto index = slots.at(point.trackId);
            if (index < table.partials && firstFrequency[index] == 0.0)
                firstFrequency[index] = point.frequencyHz;
        }
    }
    lastFrequency = firstFrequency;
    table.triples.assign(table.frames * table.partials * 3, 0.0);
    std::size_t next = 0;
    for (std::size_t source = 0; source < spectrum.frames.size() && next < table.frames; ++source)
    {
        const auto& frame = spectrum.frames[source];
        for (const auto& point : frame.points)
        {
            const auto index = slots.at(point.trackId);
            if (index < table.partials)
                lastFrequency[index] = point.frequencyHz;
        }
        const auto wanted = table.frames == 1 ? 0U : next * (spectrum.frames.size() - 1) / (table.frames - 1);
        if (source != wanted)
            continue;
        auto* row = table.triples.data() + next * table.partials * 3;
        for (std::size_t index = 0; index < table.partials; ++index)
            row[index * 3 + 1] = lastFrequency[index];
        for (const auto& point : frame.points)
        {
            const auto index = slots.at(point.trackId);
            if (index >= table.partials)
                continue;
            row[index * 3] = point.amplitude;
            row[index * 3 + 2] = point.bandwidthHz;
        }
        ++next;
    }
    return table;
}

bool writeSpectrumInRamTable(
    const std::filesystem::path& destination, const SpectrumInRamTable& table, std::string& error)
{
    if (table.frames == 0 || table.partials == 0 || table.triples.size() != table.frames * table.partials * 3)
    {
        error = "The spectrum has no partials to write";
        return false;
    }
    auto publication = AudioFilePublication::prepare(destination, ExistingAudioFilePolicy::Replace, error);
    if (publication == nullptr)
        return false;
    {
        std::ofstream output(publication->temporaryPath(), std::ios::binary | std::ios::trunc);
        output << std::setprecision(9);
        const auto width = table.partials * 3;
        for (std::size_t row = 0; row < table.frames; ++row)
        {
            for (std::size_t value = 0; value < width; ++value)
                output << (value == 0 ? "" : " ") << table.triples[row * width + value];
            output << '\n';
        }
        output.flush();
        if (!output.good())
        {
            error = "The spectrum table could not be written";
            std::string ignored;
            (void) publication->discard(ignored);
            return false;
        }
    }
    return publication->publish(error);
}
}
