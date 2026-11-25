#include "talas/dsp/NativeStft.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
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

void testConfigurationMatrixIsSharedAndExact()
{
    using talas::dsp::NativeStftConfiguration;
    using talas::dsp::SpectralWindowKind;
    for (const auto configuration : {
             NativeStftConfiguration{16, 16, SpectralWindowKind::Rectangular},
             NativeStftConfiguration{4096, 4096, SpectralWindowKind::Rectangular},
             NativeStftConfiguration{64, 32, SpectralWindowKind::Hann},
             NativeStftConfiguration{64, 16, SpectralWindowKind::Hann},
         })
    {
        require(talas::dsp::isValidNativeStftConfiguration(configuration),
                "advertised native STFT tuple should validate");
    }
    for (const auto configuration : {
             NativeStftConfiguration{8, 8, SpectralWindowKind::Rectangular},
             NativeStftConfiguration{12, 12, SpectralWindowKind::Rectangular},
             NativeStftConfiguration{64, 32, SpectralWindowKind::Rectangular},
             NativeStftConfiguration{64, 64, SpectralWindowKind::Hann},
             NativeStftConfiguration{64, 8, SpectralWindowKind::Hann},
         })
    {
        require(!talas::dsp::isValidNativeStftConfiguration(configuration),
                "tuple outside the frozen native STFT matrix should fail");
    }
}

void testForwardEmissionTimestampAndRealBinsAreExact()
{
    constexpr std::size_t size = 16;
    const talas::dsp::NativeStftConfiguration configuration{
        size,
        size,
        talas::dsp::SpectralWindowKind::Rectangular,
    };
    talas::dsp::PreparedForwardStft forward(configuration);
    talas::dsp::PreparedComplexSpectrumSequence frames(2, size / 2 + 1);

    std::array<double, size> impulse{};
    impulse[0] = 1.0;
    require(forward.process(
                std::span<const double>{impulse}.first(size - 1),
                timestamp(0),
                frames)
                == talas::dsp::NativeStftStatus::Ok,
            "incomplete forward window should be accepted");
    require(frames.frames().empty(),
            "forward should not emit before all N input samples exist");

    frames.reset();
    require(forward.process(
                std::span<const double>{impulse}.last(1),
                timestamp(size - 1),
                frames)
                == talas::dsp::NativeStftStatus::Ok,
            "sample N-1 should emit the first frame");
    require(frames.frames().size() == 1,
            "forward should emit exactly one complete frame");
    const auto& frame = frames.frames().front();
    require(frame.analysisTimestamp == timestamp(0)
                && frame.sourceSequence == 0
                && frame.frameSize == size
                && frame.hopSize == size
                && frame.window == talas::dsp::SpectralWindowKind::Rectangular
                && frame.channelIndex == 0
                && frame.bins.size() == size / 2 + 1,
            "first native frame metadata should be exact");
    for (const auto bin : frame.bins)
    {
        requireNear(bin.real(), 1.0, 1.0e-12,
                    "unit impulse should have unit real magnitude in every retained bin");
        requireNear(bin.imag(), 0.0, 1.0e-12,
                    "unit impulse should have zero imaginary bins");
    }
}

void testForwardBinCenteredSineFixesMagnitudePhaseAndOrdering()
{
    constexpr std::size_t size = 64;
    constexpr std::size_t targetBin = 5;
    const auto pi = std::acos(-1.0);
    const auto phase = pi / 4.0;
    const talas::dsp::NativeStftConfiguration configuration{
        size,
        size,
        talas::dsp::SpectralWindowKind::Rectangular,
    };
    talas::dsp::PreparedForwardStft forward(configuration);
    talas::dsp::PreparedComplexSpectrumSequence frames(1, size / 2 + 1);
    std::array<double, size> sine{};
    for (std::size_t index = 0; index < sine.size(); ++index)
    {
        sine[index] = std::sin(
            2.0 * pi * static_cast<double>(targetBin * index)
                / static_cast<double>(size)
            + phase);
    }

    require(forward.process(sine, timestamp(100), frames)
                == talas::dsp::NativeStftStatus::Ok,
            "bin-centered native sine should transform");
    require(frames.frames().size() == 1
                && frames.frames().front().analysisTimestamp
                    == timestamp(100),
            "bin-centered native sine should emit one timestamped frame");
    const auto& bins = frames.frames().front().bins;
    const auto expectedComponent = static_cast<double>(size)
        / (2.0 * std::sqrt(2.0));
    for (std::size_t bin = 0; bin < bins.size(); ++bin)
    {
        const auto expectedReal = bin == targetBin
            ? expectedComponent
            : 0.0;
        const auto expectedImaginary = bin == targetBin
            ? -expectedComponent
            : 0.0;
        requireNear(
            bins[bin].real(),
            expectedReal,
            1.0e-10,
            "native sine should retain exact real-bin ordering and magnitude");
        requireNear(
            bins[bin].imag(),
            expectedImaginary,
            1.0e-10,
            "native sine should retain the negative-exponential phase convention");
    }
}

struct CapturedFrame
{
    talas::dsp::SpectralFrameTimestamp timestamp;
    std::uint64_t sequence = 0;
    std::vector<std::complex<double>> bins;
};

std::vector<CapturedFrame> renderForwardPartitions(
    const talas::dsp::NativeStftConfiguration& configuration,
    std::span<const double> input,
    const std::vector<std::size_t>& partitions)
{
    const auto maximumBlock = *std::ranges::max_element(partitions);
    const auto capacity = talas::dsp::preparedSpectralFrameCapacity(
        maximumBlock,
        configuration.hopSize,
        1,
        0);
    require(capacity.has_value(), "forward fixture capacity should be representable");
    talas::dsp::PreparedForwardStft forward(configuration);
    talas::dsp::PreparedComplexSpectrumSequence frames(
        *capacity,
        configuration.frameSize / 2 + 1);
    std::vector<CapturedFrame> result;
    std::size_t offset = 0;
    std::size_t partitionIndex = 0;
    while (offset < input.size())
    {
        const auto count = std::min(
            partitions[partitionIndex % partitions.size()],
            input.size() - offset);
        frames.reset();
        require(forward.process(
                    input.subspan(offset, count),
                    timestamp(offset),
                    frames)
                    == talas::dsp::NativeStftStatus::Ok,
                "partitioned forward process should succeed");
        for (const auto& frame : frames.frames())
        {
            result.push_back({
                frame.analysisTimestamp,
                frame.sourceSequence,
                {frame.bins.begin(), frame.bins.end()},
            });
        }
        offset += count;
        ++partitionIndex;
    }
    return result;
}

void testForwardFramesArePartitionInvariant()
{
    const talas::dsp::NativeStftConfiguration configuration{
        64,
        16,
        talas::dsp::SpectralWindowKind::Hann,
    };
    std::vector<double> input(257);
    for (std::size_t index = 0; index < input.size(); ++index)
    {
        input[index] = 0.4 * std::sin(0.11 * static_cast<double>(index))
            + 0.2 * std::cos(0.037 * static_cast<double>(index));
    }
    const auto whole = renderForwardPartitions(
        configuration, input, {input.size()});
    const auto irregular = renderForwardPartitions(
        configuration, input, {1, 17, 3, 29, 5, 11});
    require(whole.size() == irregular.size(),
            "partition changes should not alter native frame count");
    for (std::size_t index = 0; index < whole.size(); ++index)
    {
        require(whole[index].timestamp == irregular[index].timestamp
                    && whole[index].sequence == irregular[index].sequence
                    && whole[index].bins == irregular[index].bins,
                "partition changes should not alter metadata or bins");
    }
}

std::vector<double> renderRoundTrip(
    const talas::dsp::NativeStftConfiguration& configuration,
    std::span<const double> source,
    const std::vector<std::size_t>& partitions)
{
    const auto latency = configuration.frameSize - 1;
    std::vector<double> input(
        source.size() + latency + configuration.frameSize,
        0.0);
    std::ranges::copy(source, input.begin());
    std::vector<double> output(input.size(), 0.0);
    const auto maximumBlock = *std::ranges::max_element(partitions);
    const auto frameCapacity = talas::dsp::preparedSpectralFrameCapacity(
        maximumBlock,
        configuration.hopSize,
        1,
        0);
    require(frameCapacity.has_value(), "round-trip frame capacity should be valid");
    talas::dsp::PreparedForwardStft forward(configuration);
    talas::dsp::PreparedInverseStft inverse(configuration, *frameCapacity);
    talas::dsp::PreparedComplexSpectrumSequence frames(
        *frameCapacity,
        configuration.frameSize / 2 + 1);

    std::size_t offset = 0;
    std::size_t partitionIndex = 0;
    while (offset < input.size())
    {
        const auto count = std::min(
            partitions[partitionIndex % partitions.size()],
            input.size() - offset);
        frames.reset();
        require(forward.process(
                    std::span<const double>{input}.subspan(offset, count),
                    timestamp(offset),
                    frames)
                    == talas::dsp::NativeStftStatus::Ok,
                "round-trip forward process should succeed");
        require(inverse.process(
                    frames.frames(),
                    timestamp(offset),
                    latency,
                    std::span<double>{output}.subspan(offset, count))
                    == talas::dsp::NativeStftStatus::Ok,
                "round-trip inverse process should succeed");
        offset += count;
        ++partitionIndex;
    }
    return output;
}

void testRoundTripLatencyHannOriginAndTailAreExact()
{
    constexpr std::size_t sourceFrames = 190;
    std::vector<double> source(sourceFrames);
    for (std::size_t index = 0; index < source.size(); ++index)
    {
        source[index] = 0.35 * std::sin(0.13 * static_cast<double>(index))
            + 0.17 * std::cos(0.071 * static_cast<double>(index));
    }
    source[0] = 0.75;

    for (const auto configuration : {
             talas::dsp::NativeStftConfiguration{
                 64, 64, talas::dsp::SpectralWindowKind::Rectangular},
             talas::dsp::NativeStftConfiguration{
                 64, 32, talas::dsp::SpectralWindowKind::Hann},
             talas::dsp::NativeStftConfiguration{
                 64, 16, talas::dsp::SpectralWindowKind::Hann},
         })
    {
        const auto contiguous = renderRoundTrip(
            configuration, source, {sourceFrames + 63});
        const auto irregular = renderRoundTrip(
            configuration, source, {1, 17, 3, 29, 5, 11});
        require(contiguous.size() == irregular.size(),
                "round-trip partition fixtures should retain output length");
        for (std::size_t index = 0; index < contiguous.size(); ++index)
        {
            requireNear(irregular[index], contiguous[index], 1.0e-12,
                        "round-trip output should be partition invariant");
        }

        const auto latency = configuration.frameSize - 1;
        for (std::size_t index = 0; index < latency; ++index)
        {
            requireNear(contiguous[index], 0.0, 0.0,
                        "direct native round-trip startup should be exactly N-1 zeros");
        }
        for (std::size_t sourceIndex = 0;
             sourceIndex < source.size();
             ++sourceIndex)
        {
            const auto expected = configuration.window
                    == talas::dsp::SpectralWindowKind::Hann
                && sourceIndex == 0
                ? 0.0
                : source[sourceIndex];
            const auto tolerance = configuration.window
                    == talas::dsp::SpectralWindowKind::Rectangular
                || sourceIndex >= configuration.frameSize
                ? 1.0e-9
                : 1.0e-6;
            requireNear(
                contiguous[latency + sourceIndex],
                expected,
                tolerance,
                "native round-trip should reconstruct at exactly N-1 latency");
        }
        for (std::size_t index = latency + source.size();
             index < contiguous.size();
             ++index)
        {
            requireNear(contiguous[index], 0.0, 1.0e-12,
                        "zero-fed synthesis tail should be fully drained after the delayed source");
        }
    }
}

void testInverseRejectsMalformedFrameBeforeMutation()
{
    constexpr std::size_t size = 16;
    const talas::dsp::NativeStftConfiguration configuration{
        size,
        size,
        talas::dsp::SpectralWindowKind::Rectangular,
    };
    talas::dsp::PreparedComplexSpectrumSequence frames(1, size / 2 + 1);
    std::array<std::complex<double>, size / 2 + 1> bins{};
    bins[0] = {1.0, 0.25};
    require(frames.append(
                timestamp(0),
                0,
                size,
                size,
                talas::dsp::SpectralWindowKind::Rectangular,
                0,
                bins)
                == talas::dsp::SpectralFrameStatus::Ok,
            "malformed inverse fixture should fit the generic frame envelope");
    talas::dsp::PreparedInverseStft inverse(configuration, 1);
    std::array<double, size - 1> startup{};
    require(inverse.process(
                {},
                timestamp(0),
                size - 1,
                startup)
                == talas::dsp::NativeStftStatus::Ok,
            "inverse malformed fixture should first establish the segment origin");
    std::array<double, 1> output{42.0};
    require(inverse.process(
                frames.frames(),
                timestamp(size - 1),
                size - 1,
                output)
                == talas::dsp::NativeStftStatus::NonRealEndpoint,
            "inverse should reject a materially complex DC endpoint");
    require(output[0] == 42.0,
            "malformed inverse frame should fail before output mutation");
}

void testProcessorsRejectStructuralAndCapacityFailuresBeforeMutation()
{
    constexpr std::size_t size = 16;
    constexpr std::size_t latency = size - 1;
    const talas::dsp::NativeStftConfiguration configuration{
        size,
        size,
        talas::dsp::SpectralWindowKind::Rectangular,
    };
    std::array<std::complex<double>, size / 2 + 1> bins{};
    std::ranges::fill(bins, std::complex<double>{1.0, 0.0});
    const talas::dsp::ComplexSpectrumFrame valid{
        .formatVersion = talas::dsp::complexSpectrumFormatVersion,
        .analysisTimestamp = timestamp(0),
        .sourceSequence = 0,
        .frameSize = size,
        .hopSize = size,
        .window = talas::dsp::SpectralWindowKind::Rectangular,
        .channelIndex = 0,
        .bins = bins,
    };
    const auto requireInverseFailure = [&](
        const talas::dsp::ComplexSpectrumFrame& frame,
        talas::dsp::NativeStftStatus expected,
        std::string_view context) {
        talas::dsp::PreparedInverseStft inverse(configuration, 1);
        std::array<double, size> output{};
        std::ranges::fill(output, 42.0);
        require(inverse.process(
                    std::span<const talas::dsp::ComplexSpectrumFrame>{&frame, 1},
                    timestamp(0),
                    latency,
                    output)
                    == expected,
                std::string(context) + " should return the exact failure status");
        require(std::ranges::all_of(output, [](double value) {
                    return value == 42.0;
                }),
                std::string(context)
                    + " should fail before mutating caller output");
    };

    auto malformed = valid;
    malformed.formatVersion = 0;
    requireInverseFailure(
        malformed,
        talas::dsp::NativeStftStatus::InvalidFrame,
        "invalid complex format version");
    malformed = valid;
    malformed.analysisTimestamp.transportEpoch += 1;
    requireInverseFailure(
        malformed,
        talas::dsp::NativeStftStatus::InvalidFrame,
        "mismatched frame epoch");
    malformed = valid;
    malformed.sourceSequence = 1;
    requireInverseFailure(
        malformed,
        talas::dsp::NativeStftStatus::InvalidFrame,
        "out-of-order source sequence");
    malformed = valid;
    malformed.frameSize = 32;
    requireInverseFailure(
        malformed,
        talas::dsp::NativeStftStatus::InvalidFrame,
        "mismatched frame size");
    malformed = valid;
    malformed.hopSize = 8;
    requireInverseFailure(
        malformed,
        talas::dsp::NativeStftStatus::InvalidFrame,
        "mismatched hop size");
    malformed = valid;
    malformed.window = talas::dsp::SpectralWindowKind::Hann;
    requireInverseFailure(
        malformed,
        talas::dsp::NativeStftStatus::InvalidFrame,
        "mismatched window");
    malformed = valid;
    malformed.channelIndex = 1;
    requireInverseFailure(
        malformed,
        talas::dsp::NativeStftStatus::InvalidFrame,
        "unsupported complex channel");
    malformed = valid;
    malformed.bins = std::span<const std::complex<double>>{bins}.first(
        bins.size() - 1);
    requireInverseFailure(
        malformed,
        talas::dsp::NativeStftStatus::InvalidFrame,
        "mismatched complex bin count");
    bins[1] = {
        std::numeric_limits<double>::quiet_NaN(), 0.0};
    malformed = valid;
    requireInverseFailure(
        malformed,
        talas::dsp::NativeStftStatus::InvalidFrame,
        "non-finite complex bin");
    bins[1] = {1.0, 0.0};
    bins.front() = {1.0, 0.25};
    malformed = valid;
    requireInverseFailure(
        malformed,
        talas::dsp::NativeStftStatus::NonRealEndpoint,
        "non-real DC endpoint");
    bins.front() = {1.0, 0.0};

    {
        talas::dsp::PreparedInverseStft inverse(configuration, 1);
        std::array<double, size> output{};
        std::ranges::fill(output, 42.0);
        require(inverse.process({}, timestamp(0), latency, output)
                    == talas::dsp::NativeStftStatus::MissingFrame,
                "missing due frame should fail structurally");
        require(std::ranges::all_of(output, [](double value) {
                    return value == 42.0;
                }),
                "missing frame should fail before output mutation");
    }
    {
        talas::dsp::PreparedInverseStft inverse(configuration, 1);
        std::array<double, latency> output{};
        std::ranges::fill(output, 42.0);
        require(inverse.process(
                    std::span<const talas::dsp::ComplexSpectrumFrame>{&valid, 1},
                    timestamp(0),
                    latency,
                    output)
                    == talas::dsp::NativeStftStatus::UnexpectedFrame,
                "future-call frame should fail structurally");
        require(std::ranges::all_of(output, [](double value) {
                    return value == 42.0;
                }),
                "future-call frame should fail before output mutation");
    }
    {
        talas::dsp::PreparedInverseStft inverse(configuration, 1);
        const std::array input{valid, valid};
        std::array<double, size> output{};
        require(inverse.process(input, timestamp(0), latency, output)
                    == talas::dsp::NativeStftStatus::InputCapacityExceeded,
                "inverse input capacity should fail closed");
    }
    {
        talas::dsp::PreparedInverseStft inverse(configuration, 1);
        std::array<double, size> output{};
        std::ranges::fill(output, 42.0);
        require(inverse.process(
                    {},
                    timestamp(
                        std::numeric_limits<std::uint64_t>::max()
                            - 8),
                    latency,
                    output)
                    == talas::dsp::NativeStftStatus::TimestampOverflow,
                "inverse block timestamp overflow should fail closed");
        require(std::ranges::all_of(output, [](double value) {
                    return value == 42.0;
                }),
                "inverse timestamp overflow should precede output mutation");
    }

    {
        talas::dsp::PreparedForwardStft forward(configuration);
        talas::dsp::PreparedComplexSpectrumSequence frames(1, size / 2 + 1);
        std::array<double, size * 2> input{};
        require(forward.process(input, timestamp(0), frames)
                    == talas::dsp::NativeStftStatus::OutputCapacityExceeded,
                "forward output capacity should be preflighted");
        require(frames.frames().empty(),
                "forward capacity failure should not publish a partial sequence");
        require(forward.process(
                    std::span<const double>{input}.first(size),
                    timestamp(0),
                    frames)
                    == talas::dsp::NativeStftStatus::Ok,
                "forward should recover at the same timestamp after capacity failure");
    }
    {
        talas::dsp::PreparedForwardStft forward(configuration);
        talas::dsp::PreparedComplexSpectrumSequence wrongBins(
            1,
            size / 2);
        std::array<double, size> input{};
        require(forward.process(input, timestamp(0), wrongBins)
                    == talas::dsp::NativeStftStatus::OutputSequenceRejected,
                "forward should reject a mismatched prepared bin shape before mutation");
        require(wrongBins.frames().empty(),
                "forward bin-shape rejection should not publish a frame");
        talas::dsp::PreparedComplexSpectrumSequence validShape(
            1,
            size / 2 + 1);
        require(forward.process(input, timestamp(0), validShape)
                    == talas::dsp::NativeStftStatus::Ok,
                "forward bin-shape rejection should retain recoverable state");
    }
    {
        talas::dsp::PreparedForwardStft forward(configuration);
        talas::dsp::PreparedComplexSpectrumSequence wrongChannels(
            1,
            size / 2 + 1,
            2);
        std::array<double, size> input{};
        require(forward.process(input, timestamp(0), wrongChannels)
                    == talas::dsp::NativeStftStatus::OutputSequenceRejected,
                "forward should reject a mismatched prepared channel shape before mutation");
        talas::dsp::PreparedComplexSpectrumSequence validShape(
            1,
            size / 2 + 1);
        require(forward.process(input, timestamp(0), validShape)
                    == talas::dsp::NativeStftStatus::Ok,
                "forward channel-shape rejection should retain recoverable state");
    }
    {
        talas::dsp::PreparedForwardStft forward(configuration);
        talas::dsp::PreparedComplexSpectrumSequence frames(2, size / 2 + 1);
        require(frames.append(
                    timestamp(0),
                    0,
                    size,
                    size,
                    talas::dsp::SpectralWindowKind::Rectangular,
                    0,
                    bins)
                    == talas::dsp::SpectralFrameStatus::Ok,
                "non-empty output sequence fixture should prepare");
        std::array<double, size> input{};
        require(forward.process(input, timestamp(0), frames)
                    == talas::dsp::NativeStftStatus::OutputSequenceNotEmpty,
                "forward should reject a non-empty caller sequence");
        frames.reset();
        require(forward.process(input, timestamp(0), frames)
                    == talas::dsp::NativeStftStatus::Ok,
                "non-empty sequence failure should not advance forward state");
    }
    {
        talas::dsp::PreparedForwardStft forward(configuration);
        talas::dsp::PreparedComplexSpectrumSequence frames(1, size / 2 + 1);
        std::array<double, size> input{};
        input[3] = std::numeric_limits<double>::quiet_NaN();
        require(forward.process(input, timestamp(0), frames)
                    == talas::dsp::NativeStftStatus::NonFiniteInput,
                "forward should reject non-finite input before mutation");
        input[3] = 0.0;
        require(forward.process(input, timestamp(0), frames)
                    == talas::dsp::NativeStftStatus::Ok,
                "forward should recover at the same timestamp after invalid input");
    }
    {
        talas::dsp::PreparedForwardStft forward(configuration);
        talas::dsp::PreparedComplexSpectrumSequence frames(1, size / 2 + 1);
        std::array<double, size> input{};
        input[3] = std::numeric_limits<double>::max();
        require(forward.process(input, timestamp(0), frames)
                    == talas::dsp::NativeStftStatus::NumericOverflow,
                "forward should report finite numeric overflow before mutation");
        input[3] = 0.0;
        require(forward.process(input, timestamp(0), frames)
                    == talas::dsp::NativeStftStatus::Ok,
                "forward numeric overflow should retain recoverable state");
    }
    {
        std::ranges::fill(
            bins,
            std::complex<double>{
                std::numeric_limits<double>::max() / 2.0,
                0.0});
        const auto overflowFrame = valid;
        talas::dsp::PreparedInverseStft inverse(configuration, 1);
        std::array<double, size> output{};
        std::ranges::fill(output, 42.0);
        require(inverse.process(
                    std::span<const talas::dsp::ComplexSpectrumFrame>{
                        &overflowFrame,
                        1},
                    timestamp(0),
                    latency,
                    output)
                    == talas::dsp::NativeStftStatus::NumericOverflow,
                "inverse should report finite transform overflow before output mutation");
        require(std::ranges::all_of(output, [](double value) {
                    return value == 42.0;
                }),
                "inverse numeric overflow should fail before caller-output mutation");
        std::ranges::fill(bins, std::complex<double>{1.0, 0.0});
    }
    {
        talas::dsp::PreparedForwardStft forward(configuration);
        talas::dsp::PreparedComplexSpectrumSequence frames(1, size / 2 + 1);
        std::array<double, size> input{};
        require(forward.process(
                    std::span<const double>{input}.first(size / 2),
                    timestamp(0),
                    frames)
                    == talas::dsp::NativeStftStatus::Ok,
                "forward discontinuity fixture should seed history");
        require(forward.process(
                    std::span<const double>{input}.last(size / 2),
                    timestamp(size / 2 + 1),
                    frames)
                    == talas::dsp::NativeStftStatus::DiscontinuousBlock,
                "forward should reject a timestamp gap");
        require(forward.process(
                    std::span<const double>{input}.last(size / 2),
                    timestamp(size / 2),
                    frames)
                    == talas::dsp::NativeStftStatus::Ok,
                "timestamp-gap rejection should retain recoverable forward state");
    }
    {
        talas::dsp::PreparedForwardStft forward(configuration);
        talas::dsp::PreparedComplexSpectrumSequence frames(1, size / 2 + 1);
        const std::array<double, 2> input{};
        require(forward.process(
                    input,
                    timestamp(
                        std::numeric_limits<std::uint64_t>::max()),
                    frames)
                    == talas::dsp::NativeStftStatus::TimestampOverflow,
                "forward block timestamp overflow should fail closed");
    }
}

void testInverseRejectsFiniteHannNormalizationOverflowBeforeMutation()
{
    constexpr std::size_t size = 4096;
    constexpr std::size_t latency = size - 1;
    const talas::dsp::NativeStftConfiguration configuration{
        size,
        size / 2,
        talas::dsp::SpectralWindowKind::Hann,
    };
    const auto pi = std::acos(-1.0);
    const auto amplitude =
        std::numeric_limits<double>::max()
        / (2.0 * static_cast<double>(size));
    std::vector<std::complex<double>> bins(size / 2 + 1);
    for (std::size_t bin = 0; bin < bins.size(); ++bin)
    {
        const auto phase = -2.0 * pi * static_cast<double>(bin)
            / static_cast<double>(size);
        bins[bin] = std::polar(amplitude, phase);
    }
    bins.front().imag(0.0);
    bins.back().imag(0.0);
    const talas::dsp::ComplexSpectrumFrame frame{
        .formatVersion = talas::dsp::complexSpectrumFormatVersion,
        .analysisTimestamp = timestamp(0),
        .sourceSequence = 0,
        .frameSize = size,
        .hopSize = size / 2,
        .window = talas::dsp::SpectralWindowKind::Hann,
        .channelIndex = 0,
        .bins = bins,
    };
    talas::dsp::PreparedInverseStft inverse(configuration, 1);
    std::vector<double> output(size + 1, 42.0);
    require(inverse.process(
                std::span<const talas::dsp::ComplexSpectrumFrame>{&frame, 1},
                timestamp(0),
                latency,
                output)
                == talas::dsp::NativeStftStatus::NumericOverflow,
            "inverse should reject a finite Hann normalization overflow");
    require(std::ranges::all_of(output, [](double value) {
                return value == 42.0;
            }),
            "Hann normalization overflow should precede output and state mutation");

    std::ranges::fill(bins, std::complex<double>{});
    require(inverse.process(
                std::span<const talas::dsp::ComplexSpectrumFrame>{&frame, 1},
                timestamp(0),
                latency,
                output)
                == talas::dsp::NativeStftStatus::Ok,
            "inverse should recover at the same timeline position after normalization overflow");
}

void testPreparedProcessesAllocateNothingAndResetCleanly()
{
    for (const auto configuration : {
             talas::dsp::NativeStftConfiguration{
                 16, 16, talas::dsp::SpectralWindowKind::Rectangular},
             talas::dsp::NativeStftConfiguration{
                 16, 8, talas::dsp::SpectralWindowKind::Hann},
             talas::dsp::NativeStftConfiguration{
                 16, 4, talas::dsp::SpectralWindowKind::Hann},
             talas::dsp::NativeStftConfiguration{
                 4096, 4096, talas::dsp::SpectralWindowKind::Rectangular},
             talas::dsp::NativeStftConfiguration{
                 4096, 2048, talas::dsp::SpectralWindowKind::Hann},
             talas::dsp::NativeStftConfiguration{
                 4096, 1024, talas::dsp::SpectralWindowKind::Hann},
         })
    {
        constexpr std::size_t blockSize = 17;
        const auto capacity = talas::dsp::preparedSpectralFrameCapacity(
            blockSize,
            configuration.hopSize,
            1,
            0);
        require(capacity.has_value(),
                "allocation fixture frame capacity should be valid");
        talas::dsp::PreparedForwardStft forward(configuration);
        talas::dsp::PreparedInverseStft inverse(
            configuration, *capacity);
        talas::dsp::PreparedComplexSpectrumSequence frames(
            *capacity,
            configuration.frameSize / 2 + 1);
        std::array<double, blockSize> input{};
        input[3] = 1.0;
        std::array<double, blockSize> firstOutput{};
        std::array<double, blockSize> repeatedOutput{};

        allocationCount.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_release);
        const auto firstForward = forward.process(
            input,
            timestamp(0),
            frames);
        const auto firstInverse = inverse.process(
            frames.frames(),
            timestamp(0),
            configuration.frameSize - 1,
            firstOutput);
        frames.reset();
        const auto repeatedForward = forward.process(
            input,
            timestamp(blockSize),
            frames);
        const auto repeatedInverse = inverse.process(
            frames.frames(),
            timestamp(blockSize),
            configuration.frameSize - 1,
            repeatedOutput);
        forward.reset();
        inverse.reset();
        frames.reset();
        countAllocations.store(false, std::memory_order_release);

        require(firstForward == talas::dsp::NativeStftStatus::Ok
                    && firstInverse == talas::dsp::NativeStftStatus::Ok
                    && repeatedForward == talas::dsp::NativeStftStatus::Ok
                    && repeatedInverse == talas::dsp::NativeStftStatus::Ok,
                "every admitted edge should process first and repeated prepared blocks");
        require(allocationCount.load(std::memory_order_relaxed) == 0,
                "native STFT process and reset calls must not allocate at any admitted edge");

        require(forward.process(
                    input,
                    timestamp(1000, 4, 8),
                    frames)
                    == talas::dsp::NativeStftStatus::Ok,
                "reset forward should accept a new timestamp domain");
    }
}
}

int main()
{
    testConfigurationMatrixIsSharedAndExact();
    testForwardEmissionTimestampAndRealBinsAreExact();
    testForwardBinCenteredSineFixesMagnitudePhaseAndOrdering();
    testForwardFramesArePartitionInvariant();
    testRoundTripLatencyHannOriginAndTailAreExact();
    testInverseRejectsMalformedFrameBeforeMutation();
    testProcessorsRejectStructuralAndCapacityFailuresBeforeMutation();
    testInverseRejectsFiniteHannNormalizationOverflowBeforeMutation();
    testPreparedProcessesAllocateNothingAndResetCleanly();
    return 0;
}
