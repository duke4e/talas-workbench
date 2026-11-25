#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <span>
#include <string_view>

namespace talas::test::lag_subspace
{
// Analytic input fixtures shared by the subspace and mode-extraction tests.
// Frequencies and amplitudes are scoring data, never solver inputs.
inline std::array<double, 191> sourceDifferences(std::span<const double> frequencies,
    std::span<const double> amplitudes, bool quantized = false, double center = 3.141)
{
    std::array<double, 191> differences{};
    auto sample = [&](std::size_t index)
    {
        const auto time = center + (static_cast<double>(index * 32) - 4096) / 48000;
        auto value = 0.1;
        for (std::size_t partial = 0; partial < frequencies.size(); ++partial)
            value += amplitudes[partial] * std::sin(2 * std::numbers::pi * frequencies[partial] * time
                                                  + .73 - 1.83 * partial);
        return quantized ? std::round(value * 32768) / 32768 : value;
    };
    auto previous = sample(0);
    for (std::size_t i = 0; i < differences.size(); ++i)
    {
        const auto next = sample(i + 1);
        differences[i] = next - previous;
        previous = next;
    }
    return differences;
}

template <class Callback> void forEachProfile(Callback&& run)
{
    for (const auto upper : {28.0, 40.0})
        for (const auto swapped : {false, true})
            for (const auto quantized : {false, true})
                for (const auto center : {.731, 3.141})
                {
                    const std::array frequencies{20.0, upper};
                    const auto amplitudes = swapped ? std::array{.08, .25} : std::array{.25, .08};
                    run(std::span{frequencies}, std::span{amplitudes}, quantized, center, "low_pair");
                }
    for (const auto base : {173.2, 4999.3, 13001.0})
        for (const auto ratio : {.32, .01})
            for (const auto quantized : {false, true})
            {
                const std::array frequencies{base, base + 8.4}, amplitudes{.25, .25 * ratio};
                run(std::span{frequencies}, std::span{amplitudes}, quantized, .731, "higher_pair");
            }
    const std::array dispersed{91.1, 171.3, 251.9, 332.3, 411.7, 493.1, 573.7, 653.9};
    std::array<double, 8> amplitudes{};
    for (std::size_t i = 0; i < amplitudes.size(); ++i) amplitudes[i] = .25 / (i + 1);
    for (std::size_t count = 1; count <= dispersed.size(); ++count)
        run(std::span{dispersed}.first(count), std::span{amplitudes}.first(count), false, 3.141, "dispersed");
}
}
