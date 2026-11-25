#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <optional>
#include <string>

// A Filter, whose Order is its number
// of poles. A LowPass or HighPass is Order one-pole sections at Frequency,
// which Q does not touch. An AllPass pairs its poles into two-pole sections
// at Frequency whose band Q narrows, a one-pole section taking an odd one,
// so that it shifts the phase by -90 * Order degrees at Frequency. Scale is
// the gain on the Input, and Feedback feeds the filtered signal back around
// the whole cascade, subtracted when the filter is a LowPass. The sections
// integrate trapezoidally and the loop is solved within the sample, so the
// filter is the bilinear image of its analog cascade.
//
// A high-pass at 150 hz and a low-pass at 1200 hz are expected to
// pass the band between them unattenuated, which
// Order sections each 3 dB down at the cutoff cannot do: the band's middle
// is about 4 dB down and its edges 12 dB. A Filter's Response butterworth
// makes its LowPass or HighPass Order poles a Butterworth filter instead,
// maximally flat in its passband and 3 dB down at Frequency: two-pole
// sections at Frequency whose dampings place the poles evenly on a half
// circle, and a one-pole section for an odd Order. Q does not touch them
// either; an AllPass keeps its sections. onePoleSections stays the
// default.
namespace talas::runtime::filter_poles
{
enum class Response
{
    LowPass,
    HighPass,
    AllPass,
};

// The loop gain at which Order identical one-pole sections ring: it makes
// up the cos(pi / Order) each section passes where their phases sum to 180
// degrees. One or two sections never ring; they take four's 4. Feedback has
// no documented scale; ours puts the edge of ringing at 1, so that a
// Feedback close to 1 rings.
[[nodiscard]] inline double ringingLoopGain(std::size_t order) noexcept
{
    if (order <= 2)
        return 4.0;
    return std::pow(
        std::cos(std::numbers::pi / static_cast<double>(order)),
        -static_cast<double>(order));
}

// The damping of a Butterworth filter's two-pole section `index` of
// Order poles: the sine of the angle its poles make with the imaginary
// axis, (2 index + 1) pi / (2 Order).
[[nodiscard]] inline double butterworthDamping(std::size_t order, std::size_t index) noexcept
{
    return std::sin(static_cast<double>(2 * index + 1) * std::numbers::pi
        / (2.0 * static_cast<double>(order)));
}

// The loop gain at which a Butterworth filter of Order poles rings: what
// its analog prototype passes where its phase reaches 180 degrees, Order 3
// at sqrt(2) times Frequency, Order 4 at Frequency and higher Orders within
// the passband, where it passes nearly unity. One or two poles never ring;
// they take four's sqrt(2), as the one-pole sections take four's 4.
[[nodiscard]] inline double butterworthRingingLoopGain(std::size_t order) noexcept
{
    static constexpr double gains[] = {
        3.0,
        1.4142135623730951,
        1.089987400733186,
        1.0138860631954056,
        1.0013825663571645,
        1.0000947439317813,
        1.000004821091693,
        1.0000001911748067,
        1.0000000060987722,
        1.0000000001602385,
        1.0000000000035318,
        1.0000000000000662,
        1.0000000000000011,
        1.0,
    };
    if (order <= 2 || order == 4)
        return gains[1];
    return gains[std::min<std::size_t>(order, 16) - 3];
}

// What the output is multiplied by before it is added to the Input: the
// Feedback, negated for a LowPass, so that a negative one turns the
// sign over. Subtracted feedback makes a LowPass or HighPass ring and is
// scaled by ringingLoopGain; added feedback, and an AllPass's, stays below 1,
// beyond which the filter would run away.
[[nodiscard]] inline double loopCoefficient(
    Response response,
    std::size_t order,
    double feedback,
    bool butterworth = false) noexcept
{
    const auto coefficient =
        response == Response::LowPass ? -feedback : feedback;
    if (coefficient < 0.0 && response != Response::AllPass)
    {
        return coefficient
            * (butterworth ? butterworthRingingLoopGain(order)
                           : ringingLoopGain(order));
    }
    return coefficient;
}

// One frame through Order poles held in `stages`, whose elements carry
// ic1eq and ic2eq. False, with `stages` part-way updated, when a value is
// not finite; callers keep a candidate copy. When invertsNegativeFeedback
// holds (format 92), a negative Feedback plays as the positive one 180
// degrees out of phase: the loop takes its size and the output is
// turned over. When butterworth holds, a LowPass or HighPass is a
// Butterworth filter of Order poles.
template <typename Stages>
[[nodiscard]] bool processFrame(
    Response response,
    std::size_t order,
    double sampleRate,
    double input,
    double frequency,
    double q,
    double scale,
    double feedback,
    Stages& stages,
    double& output,
    bool invertsNegativeFeedback = false,
    bool butterworth = false) noexcept
{
    const auto turnedOver = invertsNegativeFeedback && feedback < 0.0;
    if (turnedOver)
        feedback = -feedback;
    const auto g = std::tan(std::numbers::pi * frequency / sampleRate);
    const auto onePole = g / (1.0 + g);
    const auto damping = 1.0 / (2.0 * q);
    const auto denominator = 1.0 + 2.0 * damping * g + g * g;
    // A Butterworth LowPass or HighPass pairs its poles too, each pair at
    // its own damping.
    const auto flat = butterworth && response != Response::AllPass;
    const auto pairs = response == Response::AllPass || flat ? order / 2 : 0;
    const auto sections = order - pairs;
    if (!std::isfinite(g) || !std::isfinite(onePole)
        || !std::isfinite(denominator) || denominator <= 0.0)
    {
        return false;
    }
    const auto pairDamping = [&](std::size_t index) noexcept {
        return flat ? butterworthDamping(order, index) : damping;
    };

    // Each section's output is its input times a gain plus what its state
    // holds, so the cascade's is too, which solves the loop.
    auto gain = 1.0;
    auto held = 0.0;
    for (std::size_t index = 0; index < sections; ++index)
    {
        const auto& stage = stages[index];
        auto sectionGain = 0.0;
        auto sectionHeld = 0.0;
        if (index < pairs)
        {
            const auto sectionDamping = pairDamping(index);
            const auto sectionDenominator =
                1.0 + 2.0 * sectionDamping * g + g * g;
            const auto stateTerm =
                (2.0 * sectionDamping + g) * stage.ic1eq + stage.ic2eq;
            const auto bandPassHeld =
                stage.ic1eq - g * stateTerm / sectionDenominator;
            switch (response)
            {
            case Response::LowPass:
                sectionGain = g * g / sectionDenominator;
                sectionHeld = g * bandPassHeld + stage.ic2eq;
                break;
            case Response::HighPass:
                sectionGain = 1.0 / sectionDenominator;
                sectionHeld = -stateTerm / sectionDenominator;
                break;
            case Response::AllPass:
                sectionGain =
                    1.0 - 4.0 * sectionDamping * g / sectionDenominator;
                sectionHeld = -4.0 * sectionDamping * bandPassHeld;
                break;
            }
        }
        else
        {
            const auto lowPassHeld = (1.0 - onePole) * stage.ic1eq;
            switch (response)
            {
            case Response::LowPass:
                sectionGain = onePole;
                sectionHeld = lowPassHeld;
                break;
            case Response::HighPass:
                sectionGain = 1.0 - onePole;
                sectionHeld = -lowPassHeld;
                break;
            case Response::AllPass:
                sectionGain = 2.0 * onePole - 1.0;
                sectionHeld = 2.0 * lowPassHeld;
                break;
            }
        }
        gain *= sectionGain;
        held = sectionGain * held + sectionHeld;
    }
    const auto coefficient =
        loopCoefficient(response, order, feedback, flat);
    const auto solved =
        (gain * scale * input + held) / (1.0 - coefficient * gain);
    auto sample = scale * input + coefficient * solved;
    if (!std::isfinite(solved) || !std::isfinite(sample))
        return false;

    for (std::size_t index = 0; index < sections; ++index)
    {
        auto& stage = stages[index];
        if (index < pairs)
        {
            const auto sectionDamping = pairDamping(index);
            const auto highPass =
                (sample - (2.0 * sectionDamping + g) * stage.ic1eq
                    - stage.ic2eq)
                / (1.0 + 2.0 * sectionDamping * g + g * g);
            const auto bandPass = g * highPass + stage.ic1eq;
            const auto lowPass = g * bandPass + stage.ic2eq;
            const auto nextIc1eq = 2.0 * bandPass - stage.ic1eq;
            const auto nextIc2eq = 2.0 * lowPass - stage.ic2eq;
            sample = response == Response::LowPass ? lowPass
                : response == Response::HighPass
                ? highPass
                : highPass + lowPass - 2.0 * sectionDamping * bandPass;
            if (!std::isfinite(sample) || !std::isfinite(nextIc1eq)
                || !std::isfinite(nextIc2eq))
            {
                return false;
            }
            stage.ic1eq = nextIc1eq;
            stage.ic2eq = nextIc2eq;
            continue;
        }
        const auto toward = (sample - stage.ic1eq) * onePole;
        const auto lowPass = toward + stage.ic1eq;
        const auto nextIc1eq = lowPass + toward;
        switch (response)
        {
        case Response::LowPass:
            sample = lowPass;
            break;
        case Response::HighPass:
            sample -= lowPass;
            break;
        case Response::AllPass:
            sample = 2.0 * lowPass - sample;
            break;
        }
        if (!std::isfinite(sample) || !std::isfinite(nextIc1eq))
            return false;
        stage.ic1eq = nextIc1eq;
    }
    output = turnedOver ? -sample : sample;
    return true;
}
}

namespace talas::runtime
{
// A Filter's poles: "order" for one per Order, or the two per Order of
// projects before format 80, each a two-pole section with Q, with Scale on
// the output and Feedback a sample late.
[[nodiscard]] inline std::optional<bool> filterOrderPoles(
    const std::optional<std::string>& choice) noexcept
{
    if (!choice.has_value())
        return std::nullopt;
    if (*choice == "order")
        return true;
    if (*choice == "legacyTwoPerOrder")
        return false;
    return std::nullopt;
}

// A Filter's Response: "butterworth" for a maximally flat passband, or the
// Order identical one-pole sections, as projects without the field have
// played.
[[nodiscard]] inline std::optional<bool> filterButterworth(
    const std::optional<std::string>& choice) noexcept
{
    if (!choice.has_value() || *choice == "onePoleSections")
        return false;
    if (*choice == "butterworth")
        return true;
    return std::nullopt;
}

// A Filter's negative Feedback: "invertsPhase" (format 92) or the
// sign turned over of projects before it; absent, invertsPhase.
[[nodiscard]] inline std::optional<bool> filterInvertsNegativeFeedback(
    const std::optional<std::string>& choice) noexcept
{
    if (!choice.has_value() || *choice == "invertsPhase")
        return true;
    if (*choice == "legacyTurnsSignOver")
        return false;
    return std::nullopt;
}
}
