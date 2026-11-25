#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace talas::runtime::detail::cloud_bank
{
// A sounding CloudBank grain: its track, the frame it started on, and the
// frequency, length and pan read when it started.
struct Grain
{
    std::size_t track = 0;
    std::uint64_t startFrame = 0;
    double frequency = 0.0;
    double durationFrames = 0.0;
    double pan = 0.5;

    bool operator==(const Grain&) const = default;
};

// A CloudBank's grains, the first `active` of them sounding, and how many
// have started, which assigns the next one its track.
struct GrainPool
{
    std::vector<Grain> grains;
    std::size_t active = 0;
    std::uint64_t started = 0;
    // A spectrum read as an amplitude and frequency pair, its partials held
    // between blocks, packed as a Spectrum is: the count, then each partial's
    // amplitude, frequency in hertz and bandwidth.
    std::vector<double> pairedSpectrum;

    bool operator==(const GrainPool&) const = default;

    // Copies the sounding grains and cursors into a pool of the same
    // capacity without allocating.
    [[nodiscard]] bool copyStateFrom(const GrainPool& source) noexcept
    {
        if (grains.size() != source.grains.size()
            || source.active > source.grains.size()
            || pairedSpectrum.size() != source.pairedSpectrum.size())
        {
            return false;
        }
        for (std::size_t index = 0; index < source.active; ++index)
            grains[index] = source.grains[index];
        for (std::size_t index = 0; index < pairedSpectrum.size(); ++index)
            pairedSpectrum[index] = source.pairedSpectrum[index];
        active = source.active;
        started = source.started;
        return true;
    }

    // The paired spectrum is written afresh from its pair each block, so a
    // reset leaves it.
    void reset() noexcept
    {
        active = 0;
        started = 0;
    }
};
}
