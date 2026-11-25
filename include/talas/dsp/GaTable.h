#pragma once

#include <cmath>
#include <cstddef>
#include <optional>
#include <span>

namespace talas::dsp::ga
{
// Our GA analysis file (ADR-0027), the analysis a GAOscillators plays: the
// complex waveforms, 4096 samples each, then an amplitude envelope for each
// waveform, then one frequency deviation envelope, all envelopes of one length
// that varies from file to file. It is a table (a 32-bit float WAV, or any
// table file the resource loader reads) of those parts in that order after a
// header of six numbers:
//
//   magic, version, waveforms N, envelope length L, waveform length 4096,
//   base pitch in Hz,
//   N waveforms of 4096 samples, N amplitude envelopes of L frames,
//   the frequency deviation envelope of L frames (the pitch over the base
//   pitch, 1 where it holds the base pitch).
//
// TimeIndex -1 reads each envelope's first frame, 0 its middle and 1 its
// last, linearly between frames.
inline constexpr double magic = 4669745.0; // 'G', 'A', '1'
inline constexpr double version = 1.0;
inline constexpr std::size_t headerSize = 6;
inline constexpr std::size_t waveformLength = 4096;
inline constexpr std::size_t maximumWaveforms = 64;
inline constexpr std::size_t maximumEnvelopeLength = 1u << 20;

struct View
{
    std::size_t waveforms = 0;
    std::size_t envelopeLength = 0;
    double basePitch = 0.0;
    std::span<const double> table;

    [[nodiscard]] double waveform(std::size_t index, std::size_t sample) const noexcept
    {
        return table[headerSize + index * waveformLength + sample];
    }

    [[nodiscard]] double amplitude(std::size_t index, std::size_t frame) const noexcept
    {
        return table[headerSize + waveforms * waveformLength + index * envelopeLength + frame];
    }

    [[nodiscard]] double deviation(std::size_t frame) const noexcept
    {
        return table[headerSize + waveforms * (waveformLength + envelopeLength) + frame];
    }
};

// The analysis of a table read() has accepted, without checking it again.
[[nodiscard]] inline View unchecked(std::span<const double> table) noexcept
{
    View view;
    view.waveforms = static_cast<std::size_t>(table[2]);
    view.envelopeLength = static_cast<std::size_t>(table[3]);
    view.basePitch = table[5];
    view.table = table;
    return view;
}

// Whether a table starts as a GA analysis does (its magic number).
[[nodiscard]] inline bool looksLikeGa(std::span<const double> table) noexcept
{
    return !table.empty() && table[0] == magic;
}

// The analysis a table holds; nothing for one that is not a whole GA
// analysis of finite values, a positive base pitch and positive deviations.
[[nodiscard]] inline std::optional<View> read(std::span<const double> table) noexcept
{
    if (table.size() < headerSize || table[0] != magic || table[1] != version || table[4] != double(waveformLength))
        return std::nullopt;
    const auto waveforms = table[2];
    const auto length = table[3];
    if (!(waveforms >= 1.0 && waveforms <= double(maximumWaveforms) && waveforms == std::floor(waveforms))
        || !(length >= 1.0 && length <= double(maximumEnvelopeLength) && length == std::floor(length))
        || !(std::isfinite(table[5]) && table[5] > 0.0))
    {
        return std::nullopt;
    }
    View view;
    view.waveforms = static_cast<std::size_t>(waveforms);
    view.envelopeLength = static_cast<std::size_t>(length);
    view.basePitch = table[5];
    const auto size = headerSize + view.waveforms * (waveformLength + view.envelopeLength) + view.envelopeLength;
    if (table.size() != size)
        return std::nullopt;
    for (std::size_t index = headerSize; index < size; ++index)
    {
        if (!std::isfinite(table[index]))
            return std::nullopt;
    }
    view.table = table;
    for (std::size_t frame = 0; frame < view.envelopeLength; ++frame)
    {
        if (!(view.deviation(frame) > 0.0))
            return std::nullopt;
    }
    return view;
}

// An envelope's value at TimeIndex -1 to 1: frame (index + 1) / 2 * (L - 1),
// linearly between frames.
template <typename Frame>
[[nodiscard]] double atTimeIndex(std::size_t length, double timeIndex, Frame&& frame) noexcept
{
    if (length <= 1)
        return frame(std::size_t{0});
    const auto clamped = timeIndex < -1.0 ? -1.0 : timeIndex > 1.0 ? 1.0 : timeIndex;
    const auto position = (clamped + 1.0) * 0.5 * static_cast<double>(length - 1);
    const auto below = static_cast<std::size_t>(std::floor(position));
    if (below >= length - 1)
        return frame(length - 1);
    const auto fraction = position - static_cast<double>(below);
    const auto from = frame(below);
    return from + (frame(below + 1) - from) * fraction;
}
}
