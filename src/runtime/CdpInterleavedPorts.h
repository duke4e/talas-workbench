#pragma once

#include "talas/cdp/Brassage.h"
#include "talas/cdp/Cascade.h"
#include "talas/cdp/Spin.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <string_view>

namespace talas::runtime::cdp_interleaved
{
// The frames of a CDP port's interleaved output played at a time.
inline constexpr std::size_t chunkFrames = 64;

// The most channels a port plays on a node's out1 to out8.
inline constexpr std::size_t maximumOutputChannels = 8;

// CDP Cascade's modes, CDP's cascade modes 1 to 5: one channel, two, or
// eight.
[[nodiscard]] inline std::optional<talas::cdp::Cascade::Mode> cascadeMode(std::string_view choice) noexcept
{
    using Mode = talas::cdp::Cascade::Mode;
    return choice == "echoes"      ? std::optional{Mode::Echoes}
        : choice == "panRight"     ? std::optional{Mode::PanRight}
        : choice == "alternate"    ? std::optional{Mode::Alternate}
        : choice == "rotate"       ? std::optional{Mode::Rotate}
        : choice == "rotateBack"   ? std::optional{Mode::RotateBack}
                                   : std::optional<Mode>{};
}

// CDP Spin's modes, CDP's spin stereo modes 1 to 3.
[[nodiscard]] inline std::optional<talas::cdp::Spin::Mode> spinMode(std::string_view choice) noexcept
{
    using Mode = talas::cdp::Spin::Mode;
    return choice == "stereo"           ? std::optional{Mode::Stereo}
        : choice == "wide"              ? std::optional{Mode::Wide}
        : choice == "wideBareCentre"    ? std::optional{Mode::WideBareCentre}
                                        : std::optional<Mode>{};
}

// Plays a CDP port whose output interleaves its channels, one or two, into
// the left and right outputs, a stack chunk of chunkFrames at a time, so
// that it allocates nothing: process(offset, frames, interleaved) plays the
// frames from offset into interleaved. One channel goes to both outputs.
// The inputs must be as long as the outputs and finite, which is checked
// first, so that a refusal leaves the port as it was.
template <typename Process>
[[nodiscard]] bool playStereo(std::size_t channels, std::initializer_list<std::span<const double>> inputs,
    std::span<double> left, std::span<double> right, Process&& process) noexcept
{
    if (channels < 1 || channels > 2 || left.size() != right.size())
        return false;
    for (const auto input : inputs)
    {
        if (input.size() != left.size()
            || !std::ranges::all_of(input, [](double sample) { return std::isfinite(sample); }))
        {
            return false;
        }
    }
    std::array<double, 2 * chunkFrames> interleaved{};
    for (std::size_t offset = 0; offset < left.size(); offset += chunkFrames)
    {
        const auto frames = std::min(chunkFrames, left.size() - offset);
        if (!process(offset, frames, std::span<double>{interleaved.data(), frames * channels}))
            return false;
        for (std::size_t frame = 0; frame < frames; ++frame)
        {
            left[offset + frame] = interleaved[frame * channels];
            right[offset + frame] = interleaved[frame * channels + channels - 1];
        }
    }
    return true;
}

// As playStereo, for a port of one to eight channels that plays them on
// out1 to out8 too: the port's channels on the first of `outputs`, the rest
// silent; left and right play one or two channels as playStereo plays them
// and are silent with more.
template <typename Process>
[[nodiscard]] bool playStereoAndChannels(std::size_t channels,
    std::initializer_list<std::span<const double>> inputs, std::span<double> left, std::span<double> right,
    std::span<const std::span<double>> outputs, Process&& process) noexcept
{
    if (channels < 1 || channels > maximumOutputChannels || channels > outputs.size() || left.size() != right.size()
        || !std::ranges::all_of(outputs, [&](std::span<double> output) { return output.size() == left.size(); }))
    {
        return false;
    }
    for (const auto input : inputs)
    {
        if (input.size() != left.size()
            || !std::ranges::all_of(input, [](double sample) { return std::isfinite(sample); }))
        {
            return false;
        }
    }
    std::array<double, maximumOutputChannels * chunkFrames> interleaved{};
    for (std::size_t offset = 0; offset < left.size(); offset += chunkFrames)
    {
        const auto frames = std::min(chunkFrames, left.size() - offset);
        if (!process(offset, frames, std::span<double>{interleaved.data(), frames * channels}))
            return false;
        for (std::size_t frame = 0; frame < frames; ++frame)
        {
            left[offset + frame] = channels <= 2 ? interleaved[frame * channels] : 0.0;
            right[offset + frame] = channels <= 2 ? interleaved[frame * channels + channels - 1] : 0.0;
            for (std::size_t channel = 0; channel < outputs.size(); ++channel)
                outputs[channel][offset + frame] = channel < channels ? interleaved[frame * channels + channel] : 0.0;
        }
    }
    return true;
}

// CDP Brassage's modes, CDP's modify brassage modes 1, 3, 4 and 5.
[[nodiscard]] inline std::optional<talas::cdp::Brassage::Mode> brassageMode(std::string_view choice) noexcept
{
    using Mode = talas::cdp::Brassage::Mode;
    return choice == "pitchshift"  ? std::optional{Mode::Pitchshift}
        : choice == "reverb"       ? std::optional{Mode::Reverb}
        : choice == "scramble"     ? std::optional{Mode::Scramble}
        : choice == "granulate"    ? std::optional{Mode::Granulate}
                                   : std::optional<Mode>{};
}

// CDP Brassage's fields that CDP takes as breakpoint files, which may be
// live, and the static bound of each (none for amp): times in seconds.
inline constexpr std::array<std::string_view, 5> brassageLiveFields{
    "pitch", "density", "amp", "grainsize", "range"};
inline constexpr std::array<std::string_view, 5> brassageBounds{
    "highestPitch", "lowestDensity", "", "longestGrainsize", "longestRange"};

[[nodiscard]] inline bool brassageTimeField(std::size_t field) noexcept
{
    return field >= 3;
}

// Whether the mode takes brassageLiveFields[field].
[[nodiscard]] inline bool brassageTakes(talas::cdp::Brassage::Mode mode, std::size_t field) noexcept
{
    using Mode = talas::cdp::Brassage::Mode;
    switch (field)
    {
    case 0:
        return mode == Mode::Pitchshift || mode == Mode::Reverb;
    case 1:
        return mode == Mode::Reverb || mode == Mode::Granulate;
    case 2:
        return mode == Mode::Reverb;
    case 3:
        return mode == Mode::Scramble;
    case 4:
        return mode == Mode::Reverb || mode == Mode::Scramble;
    default:
        return false;
    }
}

// The node's fields as the port takes them: each of brassageLiveFields'
// starting values, whether it is live, and its bound.
struct BrassageFields
{
    talas::cdp::Brassage::Mode mode = talas::cdp::Brassage::Mode::Pitchshift;
    std::array<double, 5> values{};
    std::array<bool, 5> live{};
    std::array<double, 5> bounds{};
    double seed = 1.0;
    double memoryKilobytes = 1024.0;
};

// The port's settings, mono in, times in CDP's milliseconds: a live field the
// mode takes is bounded by its static bound (the lowest density, the highest
// of the others), which must be no nearer than its starting value, so that
// no live value moves the latency or CDP's buffers; a fixed field is its own
// bound, as CDP with a number. Nullopt for a live field past its bound, a
// seed that is not whole from 0 to 4294967295 or memory that is not whole
// kilobytes from 10 to 20480; the port checks the rest as it prepares.
[[nodiscard]] inline std::optional<talas::cdp::Brassage::Settings> brassageSettings(const BrassageFields& fields) noexcept
{
    const auto whole = [](double value, double low, double high) {
        return std::isfinite(value) && value == std::round(value) && value >= low && value <= high;
    };
    if (!whole(fields.seed, 0.0, 4294967295.0) || !whole(fields.memoryKilobytes, 10.0, 20480.0))
        return std::nullopt;
    std::array<double, 5> values{};
    std::array<double, 5> bounds{};
    for (std::size_t field = 0; field < values.size(); ++field)
    {
        const auto scale = brassageTimeField(field) ? 1000.0 : 1.0;
        const auto live = fields.live[field] && brassageTakes(fields.mode, field) && !brassageBounds[field].empty();
        values[field] = fields.values[field] * scale;
        bounds[field] = live ? fields.bounds[field] * scale : std::numeric_limits<double>::quiet_NaN();
        if (live && !(field == 1 ? bounds[field] <= values[field] : bounds[field] >= values[field]))
            return std::nullopt;
    }
    talas::cdp::Brassage::Settings settings;
    settings.mode = fields.mode;
    settings.channels = 1;
    settings.pitch = values[0];
    settings.density = values[1];
    settings.amp = values[2];
    settings.grainsizeMs = values[3];
    settings.rangeMs = values[4];
    settings.highestPitch = bounds[0];
    settings.lowestDensity = bounds[1];
    settings.longestGrainsizeMs = bounds[3];
    settings.longestRangeMs = bounds[4];
    settings.seed = static_cast<std::uint32_t>(fields.seed);
    settings.memoryKilobytes = static_cast<int>(fields.memoryKilobytes);
    return settings;
}

// A block's live fields, those given (times in seconds), for the grains that
// start from the next call on; the caller gives only those its mode takes.
[[nodiscard]] inline bool setBrassageControls(
    talas::cdp::Brassage& port, const std::array<std::optional<double>, 5>& controls) noexcept
{
    return (!controls[0].has_value() || port.setPitch(*controls[0]))
        && (!controls[1].has_value() || port.setDensity(*controls[1]))
        && (!controls[2].has_value() || port.setAmp(*controls[2]))
        && (!controls[3].has_value() || port.setGrainsize(*controls[3] * 1000.0))
        && (!controls[4].has_value() || port.setRange(*controls[4] * 1000.0));
}
}
