#include "talas/dsp/ReAnalysis.h"

#include "talas/dsp/ReResonator.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <numbers>

namespace talas::dsp
{
namespace
{
// Analyses past this many coefficients would not fit a table resource.
constexpr std::size_t maximumTableValues = std::size_t{1} << 24U;
constexpr std::size_t maximumWindowLength = std::size_t{1} << 20U;
constexpr double reflectionLimit = 1.0 - 1.0e-12;

// The reflection coefficients of a stretch's all-pole model by the
// Levinson-Durbin recursion, for A(z) = 1 + a1 z^-1 + ... whose last
// coefficient at each order is that order's reflection coefficient.
// Silence gives zeros.
void reflectionCoefficients(
    std::span<const double> autocorrelation,
    std::span<double> reflection,
    std::vector<double>& predictor,
    std::vector<double>& previous)
{
    const auto order = reflection.size();
    std::ranges::fill(reflection, 0.0);
    // A slight lift of the zero lag keeps the recursion stable on a
    // stretch that a lower order already predicts exactly.
    auto error = autocorrelation[0] * (1.0 + 1.0e-9);
    if (!(error > 0.0) || !std::isfinite(error))
        return;
    predictor.assign(order + 1, 0.0);
    predictor[0] = 1.0;
    for (std::size_t stage = 1; stage <= order; ++stage)
    {
        auto accumulator = autocorrelation[stage];
        for (std::size_t index = 1; index < stage; ++index)
            accumulator += predictor[index] * autocorrelation[stage - index];
        const auto coefficient = std::clamp(-accumulator / error, -reflectionLimit, reflectionLimit);
        if (!std::isfinite(coefficient))
            return;
        previous = predictor;
        for (std::size_t index = 1; index < stage; ++index)
            predictor[index] = previous[index] + coefficient * previous[stage - index];
        predictor[stage] = coefficient;
        reflection[stage - 1] = coefficient;
        error *= 1.0 - coefficient * coefficient;
        if (!(error > 0.0))
            return;
    }
}
}

ReAnalysisResult analyzeRe(
    std::span<const double> samples,
    double sampleRate,
    const ReAnalysisSettings& settings,
    bool withExcitation)
{
    ReAnalysisResult result;
    if (samples.empty())
        result.error = "RE analysis needs a sample to analyze";
    else if (!std::isfinite(sampleRate) || sampleRate <= 0.0)
        result.error = "RE analysis needs a positive sample rate";
    else if (settings.order < 1 || settings.order > re_resonator::maximumOrder)
        result.error = "RE analysis filter order must be from 1 to 128";
    else if (!std::isfinite(settings.updatesPerSecond) || settings.updatesPerSecond <= 0.0)
        result.error = "RE analysis needs a positive number of filter updates per second";
    else if (!std::isfinite(settings.averagingTimeSeconds) || settings.averagingTimeSeconds <= 0.0)
        result.error = "RE analysis needs a positive averaging time";
    else if (!std::ranges::all_of(samples, [](double sample) { return std::isfinite(sample); }))
        result.error = "RE analysis needs finite samples";
    if (!result.ok())
        return result;

    const auto order = settings.order;
    const auto last = samples.size() - 1;
    const auto spans = std::round(
        static_cast<double>(last) / sampleRate * settings.updatesPerSecond);
    const auto averaging = std::round(settings.averagingTimeSeconds * sampleRate);
    if (spans >= static_cast<double>(maximumTableValues / order)
        || averaging >= static_cast<double>(maximumWindowLength))
    {
        result.error = "RE analysis would make too many coefficient sets or too long an averaging time";
        return result;
    }
    const auto frameCount = static_cast<std::size_t>(spans) + 1;
    const auto windowLength = std::max(order + 1, static_cast<std::size_t>(averaging));

    std::vector<double> window(windowLength);
    for (std::size_t index = 0; index < windowLength; ++index)
    {
        const auto sine = std::sin(
            std::numbers::pi * (static_cast<double>(index) + 0.5) / static_cast<double>(windowLength));
        window[index] = sine * sine;
    }

    result.table.reserve(1 + frameCount * order);
    result.table.push_back(static_cast<double>(order));
    std::vector<double> stretch(windowLength);
    std::vector<double> autocorrelation(order + 1);
    std::vector<double> reflection(order);
    std::vector<double> predictor;
    std::vector<double> previous;
    for (std::size_t frame = 0; frame < frameCount; ++frame)
    {
        const auto centre = frameCount == 1
            ? 0.5 * static_cast<double>(last)
            : static_cast<double>(frame) * static_cast<double>(last)
                / static_cast<double>(frameCount - 1);
        const auto start = static_cast<long long>(
            std::llround(centre - 0.5 * static_cast<double>(windowLength)));
        for (std::size_t index = 0; index < windowLength; ++index)
        {
            const auto position = start + static_cast<long long>(index);
            stretch[index] = position >= 0 && position < static_cast<long long>(samples.size())
                ? samples[static_cast<std::size_t>(position)] * window[index]
                : 0.0;
        }
        for (std::size_t lag = 0; lag <= order; ++lag)
        {
            auto sum = 0.0;
            for (std::size_t index = lag; index < windowLength; ++index)
                sum += stretch[index] * stretch[index - lag];
            autocorrelation[lag] = sum;
        }
        reflectionCoefficients(autocorrelation, reflection, predictor, previous);
        result.table.insert(result.table.end(), reflection.begin(), reflection.end());
    }

    if (!withExcitation)
        return result;
    const auto table = re_resonator::parseTable(result.table);
    if (!table.has_value())
    {
        result.error = "RE analysis produced an invalid table";
        return result;
    }
    // The coefficients move as an REResonator's would with its TimeIndex
    // ramping from -1 to 1 over the sound.
    std::vector<double> coefficients(order);
    std::vector<double> state(order, 0.0);
    result.excitation.resize(samples.size());
    for (std::size_t index = 0; index < samples.size(); ++index)
    {
        const auto timeIndex = last == 0
            ? -1.0
            : -1.0 + 2.0 * static_cast<double>(index) / static_cast<double>(last);
        re_resonator::interpolate(
            *table, re_resonator::framePosition(timeIndex, table->frameCount), coefficients);
        if (!re_resonator::analyze(coefficients, state, samples[index], result.excitation[index]))
        {
            result.error = "RE analysis excitation must remain finite";
            result.excitation.clear();
            return result;
        }
    }
    return result;
}

std::string formatReTable(std::span<const double> table)
{
    std::string text = "# RE table (ADR-0022): the filter order, then one set of reflection coefficients per line\n";
    if (table.empty())
        return text;
    const auto order = static_cast<std::size_t>(std::max(table.front(), 1.0));
    std::array<char, 64> buffer{};
    const auto append = [&](double value) {
        const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
        if (error == std::errc{})
            text.append(buffer.data(), end);
    };
    append(table.front());
    text += '\n';
    for (std::size_t index = 1; index < table.size(); ++index)
    {
        append(table[index]);
        text += (index % order == 0) ? '\n' : ' ';
    }
    return text;
}
}
