#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace talas::runtime::checked_spectrum_modifier
{
struct Parameters
{
    bool select = true;
    std::size_t loTrack = 1;
    std::size_t hiTrack = 1;
    double expandedLoFrequency = 0.0;
    double expandedHiFrequency = 0.0;
    double contractedLoFrequency = 0.0;
    double contractedHiFrequency = 0.0;
    double frequencyScale = 1.0;
    double frequencyOffset = 0.0;
    double expandedLoAmplitude = 0.0;
    double expandedHiAmplitude = 0.0;
    double contractedLoAmplitude = 0.0;
    double contractedHiAmplitude = 0.0;
    double amplitudeScale = 1.0;
    double amplitudeOffset = 0.0;
    double frequencySmoothing = 0.0;
    double amplitudeSmoothing = 0.0;
    double probability = 1.0;
    double seed = 0.0;
    bool hearAll = false;
    std::span<const double> frequencyScaleProfile;
    std::span<const double> frequencyOffsetProfile;
    std::span<const double> amplitudeScaleProfile;
    std::span<const double> amplitudeOffsetProfile;
};

struct Partial
{
    double amplitude = 0.0;
    double frequency = 0.0;
    double bandwidth = 0.0;
};

enum class TransitionError
{
    None,
    InvalidEncodedCount,
    InsufficientInputCapacity,
    InsufficientOutputCapacity,
    InvalidStateCapacity,
    InvalidProbabilityProfile,
    InvalidTransformProfile,
    NonFiniteInput,
    InvalidHysteresisBounds,
    InvalidFrequencyTransform,
    InvalidAmplitudeTransform,
    InvalidSmoothing,
};

[[nodiscard]] inline std::uint64_t splitMix64(
    std::uint64_t value) noexcept
{
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U))
        * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U))
        * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] inline double deterministicUnitRandom(
    double seed,
    std::size_t partialIndex,
    std::uint64_t stream) noexcept
{
    const auto clampedSeed = std::clamp(seed, -1.0, 1.0);
    const auto normalizedSeed = (clampedSeed + 1.0) * 0.5;
    const auto seedBits = static_cast<std::uint64_t>(
        std::llround(
            normalizedSeed * 9007199254740991.0));
    auto value = seedBits
        ^ ((static_cast<std::uint64_t>(partialIndex) + 1ULL)
            * 0xd1b54a32d192ed03ULL);
    value ^= stream * 0x9fb21c651e98df25ULL;
    const auto randomBits = splitMix64(value);
    return static_cast<double>(randomBits >> 11U)
        * (1.0 / 9007199254740992.0);
}

[[nodiscard]] inline TransitionError processPartial(
    const Parameters& parameters,
    std::size_t partialIndex,
    const Partial& input,
    Partial& output,
    std::uint64_t probabilityFrameOrdinal = 0) noexcept
{
    if ((!parameters.frequencyScaleProfile.empty()
            && partialIndex >= parameters.frequencyScaleProfile.size())
        || (!parameters.frequencyOffsetProfile.empty()
            && partialIndex >= parameters.frequencyOffsetProfile.size())
        || (!parameters.amplitudeScaleProfile.empty()
            && partialIndex >= parameters.amplitudeScaleProfile.size())
        || (!parameters.amplitudeOffsetProfile.empty()
            && partialIndex >= parameters.amplitudeOffsetProfile.size()))
        return TransitionError::InvalidTransformProfile;
    if (!std::isfinite(input.amplitude)
        || !std::isfinite(input.frequency)
        || !std::isfinite(input.bandwidth))
    {
        return TransitionError::NonFiniteInput;
    }

    output = input;
    const auto oneBasedTrack = partialIndex + 1;
    auto criteriaActive = oneBasedTrack >= parameters.loTrack
        && oneBasedTrack <= parameters.hiTrack
        && input.frequency >= parameters.expandedLoFrequency
        && input.frequency <= parameters.expandedHiFrequency
        && input.amplitude >= parameters.expandedLoAmplitude
        && input.amplitude <= parameters.expandedHiAmplitude;
    if (criteriaActive && parameters.probability < 1.0)
    {
        criteriaActive = deterministicUnitRandom(
            parameters.seed,
            partialIndex,
            0x21ULL + probabilityFrameOrdinal)
            < parameters.probability;
    }
    const auto selected = parameters.select
        ? criteriaActive
        : !criteriaActive;

    if (selected)
    {
        const auto frequencyScale = parameters.frequencyScaleProfile.empty()
            ? parameters.frequencyScale
            : parameters.frequencyScaleProfile[partialIndex];
        const auto frequencyOffset = parameters.frequencyOffsetProfile.empty()
            ? parameters.frequencyOffset
            : parameters.frequencyOffsetProfile[partialIndex];
        const auto amplitudeScale = parameters.amplitudeScaleProfile.empty()
            ? parameters.amplitudeScale
            : parameters.amplitudeScaleProfile[partialIndex];
        const auto amplitudeOffset = parameters.amplitudeOffsetProfile.empty()
            ? parameters.amplitudeOffset
            : parameters.amplitudeOffsetProfile[partialIndex];
        if (!std::isfinite(frequencyScale) || frequencyScale < 0.0
            || !std::isfinite(frequencyOffset)
            || !std::isfinite(amplitudeScale)
            || !std::isfinite(amplitudeOffset))
            return TransitionError::InvalidTransformProfile;
        output.frequency = (input.frequency
            * frequencyScale)
            + frequencyOffset;
        if (!std::isfinite(output.frequency)
            || output.frequency < 0.0)
        {
            return TransitionError::InvalidFrequencyTransform;
        }

        output.amplitude = (input.amplitude
            * amplitudeScale)
            + amplitudeOffset;
        if (!std::isfinite(output.amplitude))
            return TransitionError::InvalidAmplitudeTransform;
    }
    else if (!parameters.hearAll)
    {
        output.amplitude = 0.0;
    }

    return TransitionError::None;
}

inline constexpr std::size_t maximumSpectrumPartials = 4096;

[[nodiscard]] inline TransitionError processPartialWithCriteria(
    const Parameters& parameters,
    std::size_t partialIndex,
    const Partial& input,
    bool criteriaActive,
    Partial& output,
    std::uint64_t probabilityFrameOrdinal = 0) noexcept
{
    if ((!parameters.frequencyScaleProfile.empty()
            && partialIndex >= parameters.frequencyScaleProfile.size())
        || (!parameters.frequencyOffsetProfile.empty()
            && partialIndex >= parameters.frequencyOffsetProfile.size())
        || (!parameters.amplitudeScaleProfile.empty()
            && partialIndex >= parameters.amplitudeScaleProfile.size())
        || (!parameters.amplitudeOffsetProfile.empty()
            && partialIndex >= parameters.amplitudeOffsetProfile.size()))
        return TransitionError::InvalidTransformProfile;
    if (!std::isfinite(input.amplitude)
        || !std::isfinite(input.frequency)
        || !std::isfinite(input.bandwidth))
    {
        return TransitionError::NonFiniteInput;
    }

    output = input;
    if (criteriaActive && parameters.probability < 1.0)
    {
        criteriaActive = deterministicUnitRandom(
            parameters.seed,
            partialIndex,
            0x21ULL + probabilityFrameOrdinal)
            < parameters.probability;
    }
    const auto selected = parameters.select
        ? criteriaActive
        : !criteriaActive;
    if (selected)
    {
        const auto frequencyScale = parameters.frequencyScaleProfile.empty()
            ? parameters.frequencyScale
            : parameters.frequencyScaleProfile[partialIndex];
        const auto frequencyOffset = parameters.frequencyOffsetProfile.empty()
            ? parameters.frequencyOffset
            : parameters.frequencyOffsetProfile[partialIndex];
        const auto amplitudeScale = parameters.amplitudeScaleProfile.empty()
            ? parameters.amplitudeScale
            : parameters.amplitudeScaleProfile[partialIndex];
        const auto amplitudeOffset = parameters.amplitudeOffsetProfile.empty()
            ? parameters.amplitudeOffset
            : parameters.amplitudeOffsetProfile[partialIndex];
        if (!std::isfinite(frequencyScale) || frequencyScale < 0.0
            || !std::isfinite(frequencyOffset)
            || !std::isfinite(amplitudeScale)
            || !std::isfinite(amplitudeOffset))
            return TransitionError::InvalidTransformProfile;
        output.frequency = (input.frequency
            * frequencyScale)
            + frequencyOffset;
        if (!std::isfinite(output.frequency)
            || output.frequency < 0.0)
        {
            return TransitionError::InvalidFrequencyTransform;
        }

        output.amplitude = (input.amplitude
            * amplitudeScale)
            + amplitudeOffset;
        if (!std::isfinite(output.amplitude))
            return TransitionError::InvalidAmplitudeTransform;
    }
    else if (!parameters.hearAll)
    {
        output.amplitude = 0.0;
    }
    return TransitionError::None;
}

[[nodiscard]] inline TransitionError processEncodedFrame(
    const Parameters& parameters,
    std::span<const double> input,
    std::span<double> output,
    std::size_t& encodedSize) noexcept
{
    encodedSize = 0;
    if (input.empty()
        || !std::isfinite(input[0])
        || input[0] < 0.0)
    {
        return TransitionError::InvalidEncodedCount;
    }
    const auto roundedCount = std::round(input[0]);
    if (std::abs(input[0] - roundedCount) > 1.0e-9
        || roundedCount
            > static_cast<double>(maximumSpectrumPartials))
    {
        return TransitionError::InvalidEncodedCount;
    }
    const auto partialCount = static_cast<std::size_t>(roundedCount);
    const auto requiredSize = 1 + partialCount * 3;
    if (requiredSize > input.size())
        return TransitionError::InsufficientInputCapacity;
    if (requiredSize > output.size())
        return TransitionError::InsufficientOutputCapacity;

    output[0] = static_cast<double>(partialCount);
    for (std::size_t partialIndex = 0;
         partialIndex < partialCount;
         ++partialIndex)
    {
        const auto base = 1 + partialIndex * 3;
        Partial outputPartial;
        const auto transition = processPartial(
            parameters,
            partialIndex,
            {
                .amplitude = input[base],
                .frequency = input[base + 1],
                .bandwidth = input[base + 2],
            },
            outputPartial);
        if (transition != TransitionError::None)
            return transition;
        output[base] = outputPartial.amplitude;
        output[base + 1] = outputPartial.frequency;
        output[base + 2] = outputPartial.bandwidth;
    }
    encodedSize = requiredSize;
    return TransitionError::None;
}

[[nodiscard]] inline TransitionError processEncodedFrameWithMembership(
    const Parameters& parameters,
    std::span<const double> input,
    std::span<double> output,
    std::span<const std::uint8_t> initialized,
    std::span<const std::uint8_t> membership,
    std::span<std::uint8_t> candidateInitialized,
    std::span<std::uint8_t> candidateMembership,
    std::size_t& encodedSize,
    std::uint64_t probabilityFrameOrdinal = 0,
    std::span<const double> probabilityProfile = {}) noexcept
{
    encodedSize = 0;
    if (input.empty()
        || !std::isfinite(input[0])
        || input[0] < 0.0)
    {
        return TransitionError::InvalidEncodedCount;
    }
    const auto roundedCount = std::round(input[0]);
    if (std::abs(input[0] - roundedCount) > 1.0e-9
        || roundedCount
            > static_cast<double>(maximumSpectrumPartials))
    {
        return TransitionError::InvalidEncodedCount;
    }
    const auto partialCount = static_cast<std::size_t>(roundedCount);
    const auto requiredSize = 1 + partialCount * 3;
    if (requiredSize > input.size())
        return TransitionError::InsufficientInputCapacity;
    if (requiredSize > output.size())
        return TransitionError::InsufficientOutputCapacity;
    if (initialized.size() != membership.size()
        || initialized.size() != candidateInitialized.size()
        || initialized.size() != candidateMembership.size()
        || initialized.size() < partialCount
        || initialized.size() > maximumSpectrumPartials)
    {
        return TransitionError::InvalidStateCapacity;
    }
    if (!probabilityProfile.empty()
        && (probabilityProfile.size() < partialCount
            || !std::ranges::all_of(
                probabilityProfile.first(partialCount),
                [](double value) {
                    return std::isfinite(value)
                        && value >= 0.0
                        && value <= 1.0;
                })))
    {
        return TransitionError::InvalidProbabilityProfile;
    }
    for (const auto profile : {
             parameters.frequencyScaleProfile,
             parameters.frequencyOffsetProfile,
             parameters.amplitudeScaleProfile,
             parameters.amplitudeOffsetProfile})
    {
        if (!profile.empty()
            && (profile.size() < partialCount
                || !std::ranges::all_of(
                    profile.first(partialCount),
                    [](double value) { return std::isfinite(value); })))
            return TransitionError::InvalidTransformProfile;
    }
    if (!parameters.frequencyScaleProfile.empty()
        && std::ranges::any_of(
            parameters.frequencyScaleProfile.first(partialCount),
            [](double value) { return value < 0.0; }))
        return TransitionError::InvalidTransformProfile;
    if (!std::isfinite(parameters.expandedLoFrequency)
        || !std::isfinite(parameters.expandedHiFrequency)
        || !std::isfinite(parameters.contractedLoFrequency)
        || !std::isfinite(parameters.contractedHiFrequency)
        || !std::isfinite(parameters.expandedLoAmplitude)
        || !std::isfinite(parameters.expandedHiAmplitude)
        || !std::isfinite(parameters.contractedLoAmplitude)
        || !std::isfinite(parameters.contractedHiAmplitude))
    {
        return TransitionError::InvalidHysteresisBounds;
    }

    const auto evaluatePartial = [&] (
        std::size_t partialIndex,
        bool& criteriaActive,
        Partial& outputPartial) noexcept {
        const auto base = 1 + partialIndex * 3;
        const Partial inputPartial{
            .amplitude = input[base],
            .frequency = input[base + 1],
            .bandwidth = input[base + 2],
        };
        if (!std::isfinite(inputPartial.amplitude)
            || !std::isfinite(inputPartial.frequency)
            || !std::isfinite(inputPartial.bandwidth))
        {
            return TransitionError::NonFiniteInput;
        }

        const auto useExpandedRange = initialized[partialIndex] == 0
            || membership[partialIndex] != 0;
        const auto loFrequency = useExpandedRange
            ? parameters.expandedLoFrequency
            : parameters.contractedLoFrequency;
        const auto hiFrequency = useExpandedRange
            ? parameters.expandedHiFrequency
            : parameters.contractedHiFrequency;
        const auto loAmplitude = useExpandedRange
            ? parameters.expandedLoAmplitude
            : parameters.contractedLoAmplitude;
        const auto hiAmplitude = useExpandedRange
            ? parameters.expandedHiAmplitude
            : parameters.contractedHiAmplitude;
        const auto oneBasedTrack = partialIndex + 1;
        criteriaActive = oneBasedTrack >= parameters.loTrack
            && oneBasedTrack <= parameters.hiTrack
            && inputPartial.frequency >= loFrequency
            && inputPartial.frequency <= hiFrequency
            && inputPartial.amplitude >= loAmplitude
            && inputPartial.amplitude <= hiAmplitude;
        auto effectiveParameters = parameters;
        if (!probabilityProfile.empty())
        {
            effectiveParameters.probability =
                probabilityProfile[partialIndex];
        }
        return processPartialWithCriteria(
            effectiveParameters,
            partialIndex,
            inputPartial,
            criteriaActive,
            outputPartial,
            probabilityFrameOrdinal);
    };

    for (std::size_t partialIndex = 0;
         partialIndex < partialCount;
         ++partialIndex)
    {
        bool validatedCriteriaActive = false;
        Partial validatedOutput;
        const auto transition = evaluatePartial(
            partialIndex,
            validatedCriteriaActive,
            validatedOutput);
        if (transition != TransitionError::None)
            return transition;
    }

    std::copy(
        initialized.begin(),
        initialized.end(),
        candidateInitialized.begin());
    std::copy(
        membership.begin(),
        membership.end(),
        candidateMembership.begin());
    output[0] = static_cast<double>(partialCount);
    for (std::size_t partialIndex = 0;
         partialIndex < partialCount;
         ++partialIndex)
    {
        const auto base = 1 + partialIndex * 3;
        bool criteriaActive = false;
        Partial outputPartial;
        const auto transition = evaluatePartial(
            partialIndex,
            criteriaActive,
            outputPartial);
        if (transition != TransitionError::None)
            return transition;
        candidateInitialized[partialIndex] = 1;
        candidateMembership[partialIndex] = criteriaActive ? 1 : 0;
        output[base] = outputPartial.amplitude;
        output[base + 1] = outputPartial.frequency;
        output[base + 2] = outputPartial.bandwidth;
    }
    encodedSize = requiredSize;
    return TransitionError::None;
}

[[nodiscard]] inline TransitionError processEncodedFrameWithSmoothing(
    const Parameters& parameters,
    std::span<const double> input,
    std::span<double> output,
    std::span<const std::uint8_t> initialized,
    std::span<const std::uint8_t> membership,
    std::span<std::uint8_t> candidateInitialized,
    std::span<std::uint8_t> candidateMembership,
    std::span<const double> previousFrequency,
    std::span<const double> previousAmplitude,
    std::span<double> candidateFrequency,
    std::span<double> candidateAmplitude,
    std::size_t& encodedSize,
    std::uint64_t probabilityFrameOrdinal = 0,
    std::span<const double> probabilityProfile = {}) noexcept
{
    encodedSize = 0;
    if (!std::isfinite(parameters.frequencySmoothing)
        || parameters.frequencySmoothing < 0.0
        || parameters.frequencySmoothing >= 1.0
        || !std::isfinite(parameters.amplitudeSmoothing)
        || parameters.amplitudeSmoothing < 0.0
        || parameters.amplitudeSmoothing >= 1.0)
        return TransitionError::InvalidSmoothing;
    if (previousFrequency.size() != initialized.size()
        || previousAmplitude.size() != initialized.size()
        || candidateFrequency.size() != initialized.size()
        || candidateAmplitude.size() != initialized.size())
        return TransitionError::InvalidStateCapacity;

    const auto transition = processEncodedFrameWithMembership(
        parameters, input, output, initialized, membership,
        candidateInitialized, candidateMembership, encodedSize,
        probabilityFrameOrdinal, probabilityProfile);
    if (transition != TransitionError::None)
        return transition;

    const auto partialCount = (encodedSize - 1) / 3;
    for (std::size_t partial = 0; partial < partialCount; ++partial)
    {
        if (initialized[partial] != 0
            && (!std::isfinite(previousFrequency[partial])
                || previousFrequency[partial] < 0.0
                || !std::isfinite(previousAmplitude[partial])))
            return TransitionError::InvalidSmoothing;
        const auto base = 1 + partial * 3;
        const auto frequency = initialized[partial] == 0
            ? output[base + 1]
            : std::lerp(output[base + 1], previousFrequency[partial],
                  parameters.frequencySmoothing);
        const auto amplitude = initialized[partial] == 0
            ? output[base]
            : std::lerp(output[base], previousAmplitude[partial],
                  parameters.amplitudeSmoothing);
        if (!std::isfinite(frequency) || frequency < 0.0
            || !std::isfinite(amplitude))
            return TransitionError::InvalidSmoothing;
    }

    std::copy(previousFrequency.begin(), previousFrequency.end(),
        candidateFrequency.begin());
    std::copy(previousAmplitude.begin(), previousAmplitude.end(),
        candidateAmplitude.begin());
    for (std::size_t partial = 0; partial < partialCount; ++partial)
    {
        const auto base = 1 + partial * 3;
        const auto frequency = initialized[partial] == 0
            ? output[base + 1]
            : std::lerp(output[base + 1], previousFrequency[partial],
                  parameters.frequencySmoothing);
        const auto amplitude = initialized[partial] == 0
            ? output[base]
            : std::lerp(output[base], previousAmplitude[partial],
                  parameters.amplitudeSmoothing);
        candidateFrequency[partial] = frequency;
        candidateAmplitude[partial] = amplitude;
        output[base] = amplitude;
        output[base + 1] = frequency;
    }
    return TransitionError::None;
}
}
