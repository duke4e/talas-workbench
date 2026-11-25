#include "talas/dsp/PackedRealFftAdapter.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <new>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
std::atomic<bool> countAllocations = false;
std::atomic<std::size_t> allocationCount = 0;
}

void* operator new(std::size_t size)
{
    if (countAllocations.load(std::memory_order_relaxed))
        allocationCount.fetch_add(1, std::memory_order_relaxed);
    if (auto* memory = std::malloc(size))
        return memory;
    throw std::bad_alloc{};
}

void* operator new[](std::size_t size)
{
    if (countAllocations.load(std::memory_order_relaxed))
        allocationCount.fetch_add(1, std::memory_order_relaxed);
    if (auto* memory = std::malloc(size))
        return memory;
    throw std::bad_alloc{};
}

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void requireNear(
    double actual,
    double expected,
    double tolerance,
    const std::string& message)
{
    if (std::abs(actual - expected) > tolerance)
    {
        throw std::runtime_error(
            message + ": expected " + std::to_string(expected)
            + ", got " + std::to_string(actual));
    }
}

talas::dsp::SpectralFrameTimestamp timestamp(
    std::uint64_t absoluteFrame,
    std::uint64_t epoch = 3,
    std::uint64_t discontinuity = 7)
{
    return {epoch, discontinuity, absoluteFrame};
}

void testConfigurationMatrixIsExact()
{
    using talas::dsp::PackedRealFftConfiguration;
    for (const auto configuration : {
             PackedRealFftConfiguration{1, false},
             PackedRealFftConfiguration{1, true},
             PackedRealFftConfiguration{16, false},
             PackedRealFftConfiguration{4096, true},
         })
    {
        require(talas::dsp::isValidPackedRealFftConfiguration(configuration),
                "advertised packed FFT configuration should validate");
    }
    for (const auto configuration : {
             PackedRealFftConfiguration{0, false},
             PackedRealFftConfiguration{2, false},
             PackedRealFftConfiguration{8, true},
             PackedRealFftConfiguration{12, false},
             PackedRealFftConfiguration{8192, true},
         })
    {
        require(!talas::dsp::isValidPackedRealFftConfiguration(configuration),
                "configuration outside the frozen packed FFT matrix should fail");
    }
}

void testLengthOneRemainsAnExactIdentity()
{
    for (const auto inverse : {false, true})
    {
        talas::dsp::PreparedPackedRealFftAdapter adapter({1, inverse});
        const std::array<double, 5> input{-2.0, 0.0, 1.5, 7.0, -0.25};
        std::array<double, input.size()> output{};
        require(adapter.process(input, timestamp(100), 37, output)
                    == talas::dsp::PackedRealFftStatus::Ok,
                "length-one adapter should process");
        require(std::ranges::equal(input, output),
                "length-one forward and inverse must remain exact identities");
    }
}

void testForwardPackingAndLatencyAreExact()
{
    constexpr std::size_t size = 16;
    talas::dsp::PreparedPackedRealFftAdapter forward({size, false});
    std::array<double, 2 * size - 1> input{};
    input.front() = 1.0;
    std::array<double, input.size()> output{};

    require(forward.process(input, timestamp(0), 0, output)
                == talas::dsp::PackedRealFftStatus::Ok,
            "forward impulse should process");
    for (std::size_t frame = 0; frame < size - 1; ++frame)
        require(output[frame] == 0.0,
                "forward output must remain zero before N-1 latency");
    for (std::size_t packedIndex = 0; packedIndex < size; ++packedIndex)
    {
        const auto expected = packedIndex <= 1 || packedIndex % 2 == 0
            ? 1.0
            : 0.0;
        requireNear(output[size - 1 + packedIndex], expected, 1.0e-12,
                    "unit impulse should freeze packed real and imaginary scalars");
    }

    forward.reset();
    constexpr std::size_t bin = 3;
    std::ranges::fill(input, 0.0);
    for (std::size_t sample = 0; sample < size; ++sample)
    {
        input[sample] = std::cos(
            2.0 * std::numbers::pi_v<double>
            * static_cast<double>(bin * sample)
            / static_cast<double>(size));
    }
    require(forward.process(input, timestamp(50, 4, 8), 0, output)
                == talas::dsp::PackedRealFftStatus::Ok,
            "forward bin-centered sine fixture should process");
    const auto packedRealIndex = 2 + 2 * (bin - 1);
    for (std::size_t packedIndex = 0; packedIndex < size; ++packedIndex)
    {
        const auto expected = packedIndex == packedRealIndex
            ? static_cast<double>(size) / 2.0
            : 0.0;
        requireNear(output[size - 1 + packedIndex], expected, 1.0e-10,
                    "bin-centered cosine should freeze packed magnitude and phase");
    }
}

std::vector<double> renderRoundTrip(
    std::span<const double> source,
    std::size_t frameSize,
    std::size_t inputLatency,
    std::span<const std::size_t> partitions)
{
    talas::dsp::PreparedPackedRealFftAdapter forward({frameSize, false});
    talas::dsp::PreparedPackedRealFftAdapter inverse({frameSize, true});
    std::vector<double> transformed(source.size(), 0.0);
    std::vector<double> reconstructed(source.size(), 0.0);
    std::size_t offset = 0;
    std::size_t partitionIndex = 0;
    while (offset < source.size())
    {
        const auto requested = partitions.empty()
            ? source.size() - offset
            : partitions[partitionIndex++ % partitions.size()];
        const auto count = std::min(requested, source.size() - offset);
        const auto blockStart = timestamp(offset);
        require(forward.process(
                    source.subspan(offset, count),
                    blockStart,
                    inputLatency,
                    std::span<double>{transformed}.subspan(offset, count))
                    == talas::dsp::PackedRealFftStatus::Ok,
                "partitioned forward adapter should process");
        require(inverse.process(
                    std::span<const double>{transformed}.subspan(offset, count),
                    blockStart,
                    inputLatency + frameSize - 1,
                    std::span<double>{reconstructed}.subspan(offset, count))
                    == talas::dsp::PackedRealFftStatus::Ok,
                "partitioned inverse adapter should process");
        offset += count;
    }
    return reconstructed;
}

void testRoundTripLatencyPhaseAndPartitionsAreExact()
{
    constexpr std::size_t size = 16;
    constexpr std::size_t latency = 5;
    std::vector<double> source(latency + 3 * size, 0.0);
    source[latency] = 1.0;
    for (std::size_t sample = 1; sample < size; ++sample)
        source[latency + sample] = std::sin(0.19 * static_cast<double>(sample));

    const auto contiguous = renderRoundTrip(source, size, latency, {});
    const std::array<std::size_t, 6> partitions{1, 7, 2, 11, 3, 5};
    const auto partitioned = renderRoundTrip(source, size, latency, partitions);
    require(contiguous.size() == partitioned.size(),
            "round-trip fixtures should have equal sizes");
    for (std::size_t frame = 0; frame < source.size(); ++frame)
    {
        requireNear(partitioned[frame], contiguous[frame], 1.0e-12,
                    "packed FFT output must be callback-partition invariant");
        const auto sourceFrame = latency + 2 * size - 2 <= frame
            ? frame - (2 * size - 2)
            : source.size();
        const auto expected = sourceFrame < latency + size
            ? source[sourceFrame]
            : 0.0;
        requireNear(contiguous[frame], expected, 1.0e-9,
                    "direct packed FFT pair must reconstruct at exactly 2N-2");
    }
}

void testStartupPaddingIsIgnoredBeforeTheCompiledPhaseOrigin()
{
    constexpr std::size_t size = 16;
    constexpr std::size_t inputLatency = 5;
    talas::dsp::PreparedPackedRealFftAdapter forward({size, false});
    std::array<double, inputLatency + size> input{};
    std::fill_n(
        input.begin(),
        inputLatency,
        std::numeric_limits<double>::quiet_NaN());
    input[inputLatency] = 1.0;
    std::array<double, input.size()> output{};

    require(forward.process(input, timestamp(100), inputLatency, output)
                == talas::dsp::PackedRealFftStatus::Ok,
            "startup alignment padding must be ignored before packed FFT validation");
    for (std::size_t frame = 0; frame < inputLatency + size - 1; ++frame)
        require(output[frame] == 0.0,
                "ignored startup padding must not publish packed output");
    requireNear(output.back(), 1.0, 1.0e-12,
                "first source frame should begin exactly at the compiled phase origin");
}

void testSupportedSizeMatrixRoundTrips()
{
    for (const auto size : {std::size_t{16}, std::size_t{64}, std::size_t{4096}})
    {
        std::vector<double> source(3 * size - 2, 0.0);
        source.front() = 1.0;
        const auto output = renderRoundTrip(source, size, 0, {});
        for (std::size_t frame = 0; frame < output.size(); ++frame)
        {
            requireNear(
                output[frame],
                frame == 2 * size - 2 ? 1.0 : 0.0,
                1.0e-9,
                "every admitted matrix fixture should round-trip at 2N-2");
        }
    }
}

void testResetAndFailureAreAtomic()
{
    constexpr std::size_t size = 16;
    talas::dsp::PreparedPackedRealFftAdapter forward({size, false});
    std::array<double, size / 2> firstHalf{};
    firstHalf.front() = 1.0;
    std::array<double, firstHalf.size()> firstOutput{};
    require(forward.process(firstHalf, timestamp(0), 0, firstOutput)
                == talas::dsp::PackedRealFftStatus::Ok,
            "partial frame should seed collection state");

    std::array<double, size / 2> invalidHalf{};
    invalidHalf[3] = std::numeric_limits<double>::quiet_NaN();
    std::array<double, invalidHalf.size()> invalidOutput{};
    std::ranges::fill(invalidOutput, 42.0);
    require(forward.process(invalidHalf, timestamp(size / 2), 0, invalidOutput)
                == talas::dsp::PackedRealFftStatus::NonFiniteInput,
            "non-finite input should fail before state mutation");
    require(std::ranges::all_of(invalidOutput, [](double value) {
                return value == 0.0;
            }),
            "failed processing must clear caller output rather than expose stale data");

    invalidHalf[3] = 0.0;
    require(forward.process(invalidHalf, timestamp(size / 2), 0, invalidOutput)
                == talas::dsp::PackedRealFftStatus::Ok,
            "same timestamp should recover after failure-atomic rejection");
    requireNear(invalidOutput.back(), 1.0, 1.0e-12,
                "retry should complete the original impulse frame");

    forward.reset();
    std::ranges::fill(firstOutput, 0.0);
    require(forward.process(firstHalf, timestamp(100, 4, 9), 0, firstOutput)
                == talas::dsp::PackedRealFftStatus::Ok,
            "reset should establish a new transport origin");
    std::ranges::fill(invalidHalf, 0.0);
    require(forward.process(invalidHalf, timestamp(108, 4, 9), 0, invalidOutput)
                == talas::dsp::PackedRealFftStatus::Ok,
            "new segment should complete independently after reset");
    requireNear(invalidOutput.back(), 1.0, 1.0e-12,
                "reset segment should not retain stale collection or serialization");

    std::array<double, 3> wrongOutput{};
    require(forward.process(firstHalf, timestamp(116, 4, 9), 0, wrongOutput)
                == talas::dsp::PackedRealFftStatus::InvalidBufferSize,
            "mismatched input/output spans should fail structurally");

    talas::dsp::PreparedPackedRealFftAdapter inverse({size, true});
    std::array<double, size> invalidPack{};
    invalidPack[5] = std::numeric_limits<double>::infinity();
    std::array<double, size> inverseOutput{};
    std::ranges::fill(inverseOutput, 42.0);
    require(inverse.process(invalidPack, timestamp(0), 0, inverseOutput)
                == talas::dsp::PackedRealFftStatus::NonFiniteInput,
            "inverse should reject a non-finite complete pack before mutation");
    require(std::ranges::all_of(inverseOutput, [](double value) {
                return value == 0.0;
            }),
            "inverse pack rejection should clear stale caller output");
    invalidPack[5] = 0.0;
    invalidPack[0] = 1.0;
    require(inverse.process(invalidPack, timestamp(0), 0, inverseOutput)
                == talas::dsp::PackedRealFftStatus::Ok,
            "inverse should recover at the same timestamp after invalid pack rejection");
}

// The stereo format: the left spectrum's
// Length / 2 bins and then the right's, real parts left and imaginary parts
// right, the forward transform divided by Length; each Nyquist bin sits in
// its DC bin's imaginary place.
void testStereoLayoutAndScaleAreExact()
{
    constexpr std::size_t size = 16;
    std::array<double, size> left{};
    std::array<double, size> right{};
    for (std::size_t n = 0; n < size; ++n)
    {
        const auto phase = 2.0 * std::numbers::pi * static_cast<double>(n) / size;
        left[n] = 0.5 * std::cos(3.0 * phase) + 0.25 + (n % 2 == 0 ? 0.125 : -0.125);
        right[n] = 0.25 * std::sin(5.0 * phase) - 0.5;
    }
    talas::dsp::PreparedPackedRealFftAdapter forward({size, false, true});
    std::array<double, size> leftOutput{};
    std::array<double, size> rightOutput{};
    require(forward.processStereo(left, right, timestamp(0), 0, leftOutput, rightOutput)
                == talas::dsp::PackedRealFftStatus::Ok,
            "stereo forward adapter should process");
    for (std::size_t frame = 0; frame + 1 < size; ++frame)
        require(leftOutput[frame] == 0.0 && rightOutput[frame] == 0.0,
                "stereo output should stay silent until the first frame completes");
    requireNear(leftOutput.back(), 0.25, 1.0e-12, "left DC bin should be the left mean");
    requireNear(rightOutput.back(), 0.125, 1.0e-12, "left Nyquist bin should take the DC bin's imaginary place");

    std::array<double, size> nextLeft{};
    std::array<double, size> nextRight{};
    require(forward.processStereo(left, right, timestamp(size), 0, nextLeft, nextRight)
                == talas::dsp::PackedRealFftStatus::Ok,
            "stereo forward adapter should serialize its frame");
    for (std::size_t slot = 1; slot < size; ++slot)
    {
        const auto real = slot == 3 ? 0.25 : slot == size / 2 ? -0.5 : 0.0;
        const auto imaginary = slot == size / 2 + 5 ? -0.125 : 0.0;
        requireNear(nextLeft[slot - 1], real, 1.0e-12,
                    "stereo slot " + std::to_string(slot) + " real part");
        requireNear(nextRight[slot - 1], imaginary, 1.0e-12,
                    "stereo slot " + std::to_string(slot) + " imaginary part");
    }
}

std::array<std::vector<double>, 2> renderStereoRoundTrip(
    const std::array<std::vector<double>, 2>& source,
    std::size_t frameSize,
    std::span<const std::size_t> partitions)
{
    talas::dsp::PreparedPackedRealFftAdapter forward({frameSize, false, true});
    talas::dsp::PreparedPackedRealFftAdapter inverse({frameSize, true, true});
    const auto frames = source[0].size();
    std::array<std::vector<double>, 2> spectrum{std::vector<double>(frames), std::vector<double>(frames)};
    std::array<std::vector<double>, 2> output{std::vector<double>(frames), std::vector<double>(frames)};
    std::size_t offset = 0;
    std::size_t partitionIndex = 0;
    while (offset < frames)
    {
        const auto count = std::min(
            partitions.empty() ? frames : partitions[partitionIndex++ % partitions.size()],
            frames - offset);
        const auto span = [&](std::vector<double>& channel) {
            return std::span<double>{channel}.subspan(offset, count);
        };
        const auto view = [&](const std::vector<double>& channel) {
            return std::span<const double>{channel}.subspan(offset, count);
        };
        require(forward.processStereo(view(source[0]), view(source[1]), timestamp(offset), 0,
                    span(spectrum[0]), span(spectrum[1]))
                    == talas::dsp::PackedRealFftStatus::Ok,
                "partitioned stereo forward adapter should process");
        require(inverse.processStereo(view(spectrum[0]), view(spectrum[1]), timestamp(offset),
                    frameSize - 1, span(output[0]), span(output[1]))
                    == talas::dsp::PackedRealFftStatus::Ok,
                "partitioned stereo inverse adapter should process");
        offset += count;
    }
    return output;
}

void testStereoRoundTripIsExactAndPartitionInvariant()
{
    constexpr std::size_t size = 16;
    std::array<std::vector<double>, 2> source{std::vector<double>(4 * size), std::vector<double>(4 * size)};
    for (std::size_t n = 0; n < 3 * size; ++n)
    {
        source[0][n] = std::sin(0.19 * static_cast<double>(n)) * 0.75;
        source[1][n] = std::cos(0.41 * static_cast<double>(n)) * -0.5 + 0.125;
    }
    const auto contiguous = renderStereoRoundTrip(source, size, {});
    const std::array<std::size_t, 6> partitions{1, 7, 2, 11, 3, 5};
    const auto partitioned = renderStereoRoundTrip(source, size, partitions);
    for (std::size_t channel = 0; channel < 2; ++channel)
    {
        for (std::size_t frame = 0; frame < source[0].size(); ++frame)
        {
            requireNear(partitioned[channel][frame], contiguous[channel][frame], 1.0e-12,
                        "stereo FFT output must be callback-partition invariant");
            const auto expected = frame >= 2 * size - 2 ? source[channel][frame - (2 * size - 2)] : 0.0;
            requireNear(contiguous[channel][frame], expected, 1.0e-12,
                        "stereo FFT pair must give back channel " + std::to_string(channel) + " at 2N-2");
        }
    }
}

void testStereoAndPackedEntryPointsStaySeparate()
{
    constexpr std::size_t size = 16;
    std::array<double, size> input{};
    std::array<double, size> output{};
    std::array<double, size> rightOutput{};
    std::ranges::fill(output, 42.0);
    talas::dsp::PreparedPackedRealFftAdapter stereo({size, false, true});
    require(stereo.process(input, timestamp(0), 0, output) == talas::dsp::PackedRealFftStatus::InvalidBufferSize
                && std::ranges::all_of(output, [](double value) { return value == 0.0; }),
            "a stereo adapter should refuse the one-channel entry point and clear its output");
    talas::dsp::PreparedPackedRealFftAdapter packed({size, false});
    require(packed.processStereo(input, input, timestamp(0), 0, output, rightOutput)
                == talas::dsp::PackedRealFftStatus::InvalidBufferSize,
            "a packed adapter should refuse the stereo entry point");
    std::array<double, 3> shortRight{};
    require(stereo.processStereo(input, shortRight, timestamp(0), 0, output, rightOutput)
                == talas::dsp::PackedRealFftStatus::InvalidBufferSize,
            "stereo channels of different lengths should fail structurally");
}

void testStereoCheckpointCopiesBothChannels()
{
    constexpr std::size_t size = 16;
    std::array<double, size / 2> left{};
    std::array<double, size / 2> right{};
    left.front() = 1.0;
    right[1] = -0.5;
    talas::dsp::PreparedPackedRealFftAdapter original({size, false, true});
    talas::dsp::PreparedPackedRealFftAdapter restored({size, false, true});
    std::array<double, size / 2> leftOutput{};
    std::array<double, size / 2> rightOutput{};
    require(original.processStereo(left, right, timestamp(0), 0, leftOutput, rightOutput)
                    == talas::dsp::PackedRealFftStatus::Ok
                && restored.copyProcessingStateFrom(original),
            "half a stereo frame should checkpoint");
    std::array<double, size / 2> silence{};
    std::array<std::array<double, size / 2>, 4> outputs{};
    require(original.processStereo(silence, silence, timestamp(size / 2), 0, outputs[0], outputs[1])
                    == talas::dsp::PackedRealFftStatus::Ok
                && restored.processStereo(silence, silence, timestamp(size / 2), 0, outputs[2], outputs[3])
                    == talas::dsp::PackedRealFftStatus::Ok,
            "both adapters should complete the frame");
    require(outputs[0] == outputs[2] && outputs[1] == outputs[3]
                && std::abs(outputs[0].back() - 1.0 / size) < 1.0e-12
                && std::abs(outputs[0].back() - outputs[2].back()) == 0.0,
            "a restored stereo adapter should continue both channels exactly");
}

void testPreparedProcessingDoesNotAllocate()
{
    constexpr std::size_t size = 16;
    talas::dsp::PreparedPackedRealFftAdapter forward({size, false});
    talas::dsp::PreparedPackedRealFftAdapter inverse({size, true});
    std::array<double, 2 * size> input{};
    input.front() = 1.0;
    std::array<double, input.size()> packed{};
    std::array<double, input.size()> output{};

    talas::dsp::PreparedPackedRealFftAdapter stereoForward({size, false, true});
    talas::dsp::PreparedPackedRealFftAdapter stereoInverse({size, true, true});
    std::array<double, input.size()> rightPacked{};
    std::array<double, input.size()> rightOutput{};

    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_release);
    forward.reset();
    inverse.reset();
    const auto forwardStatus = forward.process(input, timestamp(0), 0, packed);
    const auto inverseStatus = inverse.process(
        packed,
        timestamp(0),
        size - 1,
        output);
    const auto stereoForwardStatus = stereoForward.processStereo(
        input, input, timestamp(0), 0, packed, rightPacked);
    const auto stereoInverseStatus = stereoInverse.processStereo(
        packed, rightPacked, timestamp(0), size - 1, output, rightOutput);
    countAllocations.store(false, std::memory_order_release);

    require(forwardStatus == talas::dsp::PackedRealFftStatus::Ok
                && inverseStatus == talas::dsp::PackedRealFftStatus::Ok
                && stereoForwardStatus == talas::dsp::PackedRealFftStatus::Ok
                && stereoInverseStatus == talas::dsp::PackedRealFftStatus::Ok,
            "prepared allocation fixture should process");
    require(allocationCount.load(std::memory_order_relaxed) == 0,
            "prepared packed FFT reset and processing must not allocate");
}
}

int main()
{
    testConfigurationMatrixIsExact();
    testLengthOneRemainsAnExactIdentity();
    testForwardPackingAndLatencyAreExact();
    testRoundTripLatencyPhaseAndPartitionsAreExact();
    testStartupPaddingIsIgnoredBeforeTheCompiledPhaseOrigin();
    testSupportedSizeMatrixRoundTrips();
    testResetAndFailureAreAtomic();
    testStereoLayoutAndScaleAreExact();
    testStereoRoundTripIsExactAndPartitionInvariant();
    testStereoAndPackedEntryPointsStaySeparate();
    testStereoCheckpointCopiesBothChannels();
    testPreparedProcessingDoesNotAllocate();
    return 0;
}
