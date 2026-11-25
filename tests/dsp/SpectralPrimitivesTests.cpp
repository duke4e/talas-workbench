#include "talas/dsp/Radix2Fft.h"
#include "talas/dsp/SerializedSpectrumFrameAssembler.h"
#include "talas/dsp/SerializedSpectrumFrameSynchronizer.h"
#include "talas/dsp/SpectralFrame.h"
#include "talas/dsp/SpectralWindow.h"

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
#include <type_traits>
#include <utility>
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

template <typename Value>
void requireNear(
    Value actual,
    Value expected,
    Value tolerance,
    const std::string& message)
{
    if (std::abs(actual - expected) > tolerance)
        throw std::runtime_error(message);
}

void testSupportedFftSizesAreExplicit()
{
    static_assert(!std::is_copy_constructible_v<
                  talas::dsp::PreparedPartialSpectrumSequence>);
    static_assert(!std::is_move_constructible_v<
                  talas::dsp::PreparedComplexSpectrumSequence>);
    for (const auto size : {16U, 32U, 64U, 1024U, 4096U})
        require(talas::dsp::isSupportedRadix2FftSize(size),
                "admitted radix-2 size should be supported");
    for (const auto size : {0U, 1U, 8U, 12U, 8192U})
        require(!talas::dsp::isSupportedRadix2FftSize(size),
                "size outside 16..4096 powers of two should be rejected");

    bool rejected = false;
    try
    {
        const talas::dsp::Radix2FftPlan invalid(12);
        static_cast<void>(invalid);
    }
    catch (const std::invalid_argument&)
    {
        rejected = true;
    }
    require(rejected, "FFT plan should reject a non-power-of-two size");
}

void testImpulseAndSineFixTheFftConvention()
{
    constexpr std::size_t size = 64;
    talas::dsp::Radix2FftPlan plan(size);
    std::vector<std::complex<double>> impulse(size);
    impulse.front() = 1.0;
    require(plan.forward(impulse), "impulse forward transform should run");
    for (const auto value : impulse)
        requireNear(std::abs(value - std::complex<double>{1.0, 0.0}),
                    0.0,
                    1.0e-12,
                    "unscaled impulse spectrum should contain unit bins");
    require(plan.inverse(impulse), "impulse inverse transform should run");
    requireNear(impulse.front().real(), 1.0, 1.0e-12,
                "inverse should normalize by one over N");
    for (std::size_t index = 1; index < impulse.size(); ++index)
        requireNear(std::abs(impulse[index]), 0.0, 1.0e-12,
                    "inverse impulse tail should be zero");
    require(!plan.forward(
                std::span<std::complex<double>>{impulse}.first(size / 2)),
            "process call should reject a span that does not match its plan");

    constexpr std::size_t bin = 5;
    std::vector<std::complex<double>> sine(size);
    const auto twoPi = 2.0 * std::acos(-1.0);
    for (std::size_t index = 0; index < size; ++index)
    {
        sine[index] = std::sin(
            twoPi * static_cast<double>(bin * index)
            / static_cast<double>(size));
    }
    require(plan.forward(sine), "bin-centered sine transform should run");
    requireNear(sine[bin].real(), 0.0, 1.0e-10,
                "positive sine bin real component should be zero");
    requireNear(sine[bin].imag(), -static_cast<double>(size) / 2.0,
                1.0e-10,
                "forward transform should use the negative exponential");
    requireNear(sine[size - bin].imag(), static_cast<double>(size) / 2.0,
                1.0e-10,
                "real sine should retain conjugate symmetry");
}

void testRoundTripPrecisionAndOutOfPlaceTransform()
{
    for (const auto size : {
             16U,
             32U,
             64U,
             128U,
             256U,
             512U,
             1024U,
             2048U,
             4096U,
         })
    {
        talas::dsp::Radix2FftPlan plan(size);
        std::vector<std::complex<double>> original(size);
        for (std::size_t index = 0; index < size; ++index)
        {
            original[index] = {
                std::sin(static_cast<double>(index) * 0.173)
                    + std::cos(static_cast<double>(index) * 0.031),
                std::sin(static_cast<double>(index) * 0.071),
            };
        }
        auto transformed = original;
        require(plan.forward(transformed) && plan.inverse(transformed),
                "double round trip should run");
        double maximumError = 0.0;
        for (std::size_t index = 0; index < size; ++index)
            maximumError = std::max(
                maximumError,
                std::abs(transformed[index] - original[index]));
        require(maximumError <= 1.0e-9,
                "double FFT round trip should remain within 1e-9");

        std::vector<std::complex<double>> output(size);
        require(plan.forward(original, output),
                "out-of-place forward transform should run");
        require(plan.inverse(output),
                "out-of-place result should be invertible");
        for (std::size_t index = 0; index < size; ++index)
            requireNear(std::abs(output[index] - original[index]),
                        0.0,
                        1.0e-9,
                        "out-of-place round trip should match input");
    }

    constexpr std::size_t floatSize = 1024;
    talas::dsp::Radix2FftPlan floatPlan(floatSize);
    std::vector<std::complex<float>> original(floatSize);
    for (std::size_t index = 0; index < floatSize; ++index)
        original[index] = std::sin(static_cast<float>(index) * 0.019F);
    auto transformed = original;
    require(floatPlan.forward(transformed) && floatPlan.inverse(transformed),
            "float round trip should run");
    float maximumError = 0.0F;
    for (std::size_t index = 0; index < floatSize; ++index)
        maximumError = std::max(
            maximumError,
            std::abs(transformed[index] - original[index]));
    require(maximumError <= 1.0e-6F,
            "float FFT round trip should remain within 1e-6");

    std::array<std::complex<double>, 17> overlappingDouble{};
    const talas::dsp::Radix2FftPlan overlapPlan(16);
    require(!overlapPlan.forward(
                std::span<const std::complex<double>>{overlappingDouble}
                    .first<16>(),
                std::span<std::complex<double>>{overlappingDouble}
                    .subspan<1, 16>()),
            "out-of-place double transform should reject partial overlap");
    std::array<std::complex<float>, 17> overlappingFloat{};
    require(!overlapPlan.inverse(
                std::span<const std::complex<float>>{overlappingFloat}
                    .subspan<1, 16>(),
                std::span<std::complex<float>>{overlappingFloat}
                    .first<16>()),
            "out-of-place float transform should reject reverse partial overlap");
}

void testWindowsAndColaMatrixAreDeterministic()
{
    constexpr std::size_t size = 64;
    const auto rectangular = talas::dsp::makeSpectralWindow(
        talas::dsp::SpectralWindowKind::Rectangular,
        size);
    const auto hann = talas::dsp::makeSpectralWindow(
        talas::dsp::SpectralWindowKind::Hann,
        size);
    require(rectangular.size() == size && hann.size() == size,
            "window generation should retain frame size");
    requireNear(hann.front(), 0.0, 1.0e-12,
                "periodic Hann should begin at zero");
    requireNear(hann[size / 2], 1.0, 1.0e-12,
                "periodic Hann should peak halfway through the frame");

    const auto rectangularCola = talas::dsp::validateConstantOverlapAdd(
        rectangular,
        size);
    const auto hannHalf = talas::dsp::validateConstantOverlapAdd(
        hann,
        size / 2);
    const auto hannQuarter = talas::dsp::validateConstantOverlapAdd(
        hann,
        size / 4);
    require(rectangularCola.valid && hannHalf.valid && hannQuarter.valid,
            "initial rectangular and Hann hop matrix should pass COLA");
    requireNear(rectangularCola.constantSum, 1.0, 1.0e-12,
                "rectangular COLA sum should be one");
    requireNear(hannHalf.constantSum, 1.0, 1.0e-12,
                "half-hop Hann COLA sum should be one");
    requireNear(hannQuarter.constantSum, 2.0, 1.0e-12,
                "quarter-hop Hann COLA sum should be two");
    require(!talas::dsp::validateConstantOverlapAdd(hann, 13).valid,
            "non-divisor hop should fail COLA validation");
    require(talas::dsp::isSupportedNativeFrameConfiguration(
                size,
                size / 2,
                talas::dsp::SpectralWindowKind::Hann)
                && !talas::dsp::isSupportedNativeFrameConfiguration(
                    size,
                    size / 3,
                    talas::dsp::SpectralWindowKind::Hann),
            "native frame support should expose only the admitted matrix");

    for (const auto supportedSize : {
             16U,
             32U,
             64U,
             128U,
             256U,
             512U,
             1024U,
             2048U,
             4096U,
         })
    {
        const auto supportedRectangular = talas::dsp::makeSpectralWindow(
            talas::dsp::SpectralWindowKind::Rectangular,
            supportedSize);
        const auto supportedHann = talas::dsp::makeSpectralWindow(
            talas::dsp::SpectralWindowKind::Hann,
            supportedSize);
        require(talas::dsp::validateConstantOverlapAdd(
                    supportedRectangular,
                    supportedSize)
                    .valid
                    && talas::dsp::validateConstantOverlapAdd(
                        supportedHann,
                        supportedSize / 2)
                        .valid
                    && talas::dsp::validateConstantOverlapAdd(
                        supportedHann,
                        supportedSize / 4)
                        .valid,
                "every advertised native size/window/hop tuple should pass COLA");
    }
}

void testBoundedPartialFramesValidateOrderingAndCapacity()
{
    talas::dsp::PreparedPartialSpectrumSequence sequence(2, 2);
    const std::array<talas::dsp::PartialSpectrumPoint, 2> partials{{
        {true, 7, 0.5, 440.0, 10.0},
        {false, 0, 0.25, 880.0, 20.0},
    }};
    const talas::dsp::SpectralFrameTimestamp timestamp{2, 3, 100};
    require(sequence.append(timestamp, 4, partials)
                == talas::dsp::SpectralFrameStatus::Ok,
            "first partial frame should append");
    require(sequence.append(
                timestamp,
                5,
                std::span<const talas::dsp::PartialSpectrumPoint>{partials}
                    .first<1>())
                == talas::dsp::SpectralFrameStatus::Ok,
            "equal timestamps should retain increasing source sequence");
    require(sequence.append({2, 3, 101}, 6, partials)
                == talas::dsp::SpectralFrameStatus::CapacityExceeded,
            "prepared partial sequence should stop at exact capacity");
    require(sequence.frames().size() == 2
                && sequence.frames()[0].formatVersion == 1
                && sequence.frames()[0].partials[0].trackId == 7,
            "partial frame views should retain version and payload");

    sequence.reset();
    require(sequence.append({2, 3, 99}, 3, partials)
                == talas::dsp::SpectralFrameStatus::Ok,
            "reset should clear ordering history without reallocating");
    require(sequence.append({2, 3, 98}, 4, partials)
                == talas::dsp::SpectralFrameStatus::OutOfOrder,
            "timestamps must remain lexicographically ordered");

    auto invalid = partials;
    invalid[0].amplitude = std::numeric_limits<double>::infinity();
    require(sequence.append({2, 3, 100}, 5, invalid)
                == talas::dsp::SpectralFrameStatus::InvalidFrame,
            "non-finite partial payload should fail closed");
}

void testBoundedComplexFramesValidateMetadataAndBins()
{
    constexpr std::size_t size = 16;
    bool rejectedEmptyBus = false;
    try
    {
        const talas::dsp::PreparedComplexSpectrumSequence emptyBus(0, 0, 0);
        static_cast<void>(emptyBus);
    }
    catch (const std::invalid_argument&)
    {
        rejectedEmptyBus = true;
    }
    require(rejectedEmptyBus,
            "prepared complex frame storage should require a positive bus width");

    talas::dsp::PreparedComplexSpectrumSequence sequence(2, size / 2 + 1);
    std::array<std::complex<double>, size / 2 + 1> bins{};
    bins[1] = {3.0, -2.0};
    require(sequence.append(
                {1, 0, 32},
                9,
                size,
                size / 2,
                talas::dsp::SpectralWindowKind::Hann,
                0,
                bins)
                == talas::dsp::SpectralFrameStatus::Ok,
            "valid complex frame should append");
    require(sequence.frames().front().bins.size() == bins.size()
                && sequence.frames().front().bins[1] == bins[1],
            "complex frame should retain an immutable prepared slice");
    require(sequence.append(
                {1, 0, 48},
                10,
                size,
                size / 2,
                talas::dsp::SpectralWindowKind::Hann,
                0,
                std::span<const std::complex<double>>{bins}.first(8))
                == talas::dsp::SpectralFrameStatus::InvalidFrame,
            "complex frame should require exactly N over two plus one bins");
    require(sequence.append(
                {1, 0, 48},
                10,
                size,
                size / 2,
                talas::dsp::SpectralWindowKind::Hann,
                1,
                bins)
                == talas::dsp::SpectralFrameStatus::InvalidFrame,
            "complex frame channel index should stay inside the prepared bus");
    bins[2] = {std::numeric_limits<double>::quiet_NaN(), 0.0};
    require(sequence.append(
                {1, 0, 48},
                10,
                size,
                size / 2,
                talas::dsp::SpectralWindowKind::Hann,
                0,
                bins)
                == talas::dsp::SpectralFrameStatus::InvalidFrame,
            "non-finite complex bins should fail closed");
}

void testCapacityAndLegacyPartialCompatibilityAreBounded()
{
    const auto capacity = talas::dsp::preparedSpectralFrameCapacity(10, 4, 1, 5);
    require(capacity.has_value() && *capacity == 6,
            "capacity should include callback, boundary, and tail frames");
    require(!talas::dsp::preparedSpectralFrameCapacity(10, 0, 1, 0).has_value(),
            "zero minimum hop should be rejected");
    require(!talas::dsp::preparedSpectralFrameCapacity(
                 std::numeric_limits<std::size_t>::max(),
                 1,
                 1,
                 0)
                 .has_value(),
            "capacity arithmetic should fail closed on overflow");

    bool rejectedStorageOverflow = false;
    try
    {
        const talas::dsp::PreparedPartialSpectrumSequence impossible(
            std::numeric_limits<std::size_t>::max(),
            2);
        static_cast<void>(impossible);
    }
    catch (const std::length_error&)
    {
        rejectedStorageOverflow = true;
    }
    require(rejectedStorageOverflow,
            "prepared frame storage should reject multiplication overflow before allocation");

    const std::array<talas::dsp::PartialSpectrumPoint, 2> partials{{
        {true, 9, 0.5, 440.0, 10.0},
        {false, 0, 0.25, 880.0, 20.0},
    }};
    const talas::dsp::PartialSpectrumFrame frame{
        1,
        {3, 4, 128},
        7,
        partials,
    };
    std::array<double, 7> encoded{};
    std::size_t written = 0;
    require(talas::dsp::encodeLegacyPartialSpectrum(frame, encoded, written)
                == talas::dsp::SpectralFrameStatus::Ok
                && written == encoded.size()
                && encoded[0] == 2.0,
            "legacy encoder should write count plus triples");
    std::array<talas::dsp::PartialSpectrumPoint, 2> decoded{};
    std::size_t decodedCount = 0;
    require(talas::dsp::decodeLegacyPartialSpectrum(
                encoded,
                decoded,
                decodedCount)
                == talas::dsp::SpectralFrameStatus::Ok
                && decodedCount == 2
                && !decoded[0].hasTrackId
                && decoded[1].frequencyHz == 880.0,
            "legacy decoder should restore finite triples without track IDs");
    encoded[0] = 3.0;
    require(talas::dsp::decodeLegacyPartialSpectrum(
                encoded,
                decoded,
                decodedCount)
                == talas::dsp::SpectralFrameStatus::InvalidFrame,
            "legacy decoder should reject mismatched count and payload size");
}

void testSerializedSpectrumSyncOutputStartsAtComponentZero()
{
    using talas::dsp::PreparedSerializedSpectrumFrameSynchronizer;
    using talas::dsp::SerializedSpectrumSyncStatus;
    const std::array amplitudes{10.0, 11.0, 12.0, 13.0,
                                20.0, 21.0, 22.0, 23.0};
    const std::array frequencies{110.0, 111.0, 112.0, 113.0,
                                 120.0, 121.0, 122.0, 123.0};
    const std::array syncOutput{0.0, 0.0, 1.0, 0.0,
                                0.0, 0.0, 0.0, 0.0};
    const std::array syncInput{0.0, 0.0, 0.0, 0.0,
                               0.0, 0.0, 0.0, 0.0};
    const std::array expectedAmplitude{10.0, 11.0, 10.0, 11.0,
                                       12.0, 13.0, 20.0, 21.0};
    const std::array expectedFrequency{110.0, 111.0, 110.0, 111.0,
                                       112.0, 113.0, 120.0, 121.0};
    const std::array<std::size_t, 8> expectedComponentIndex{
        0, 1, 0, 1, 2, 3, 0, 1};
    std::array<double, 8> wholeAmplitude{};
    std::array<double, 8> wholeFrequency{};
    std::array<std::size_t, 8> wholeComponentIndex{};
    PreparedSerializedSpectrumFrameSynchronizer whole(4, 8);
    require(whole.process(amplitudes, frequencies, syncOutput, syncInput,
                wholeAmplitude, wholeFrequency, wholeComponentIndex)
                == SerializedSpectrumSyncStatus::Ok
            && wholeAmplitude == expectedAmplitude
            && wholeFrequency == expectedFrequency
            && wholeComponentIndex == expectedComponentIndex,
        "SyncOutput should replay component zero at the trigger and retain paired streams");

    PreparedSerializedSpectrumFrameSynchronizer partitioned(4, 3);
    std::array<double, 8> splitAmplitude{};
    std::array<double, 8> splitFrequency{};
    std::array<std::size_t, 8> splitComponentIndex{};
    for (const auto [start, count] :
        {std::pair<std::size_t, std::size_t>{0, 3}, {3, 2}, {5, 3}})
    {
        require(partitioned.process(
                    std::span{amplitudes}.subspan(start, count),
                    std::span{frequencies}.subspan(start, count),
                    std::span{syncOutput}.subspan(start, count),
                    std::span{syncInput}.subspan(start, count),
                    std::span{splitAmplitude}.subspan(start, count),
                    std::span{splitFrequency}.subspan(start, count),
                    std::span{splitComponentIndex}.subspan(start, count))
                    == SerializedSpectrumSyncStatus::Ok,
            "partitioned SyncOutput should process each bounded block");
    }
    require(splitAmplitude == wholeAmplitude
            && splitFrequency == wholeFrequency
            && splitComponentIndex == wholeComponentIndex,
        "SyncOutput must preserve component phase across irregular blocks");

    auto inPlaceAmplitude = amplitudes;
    auto inPlaceFrequency = frequencies;
    std::array<std::size_t, 8> inPlaceComponentIndex{};
    PreparedSerializedSpectrumFrameSynchronizer inPlace(4, 8);
    require(inPlace.process(inPlaceAmplitude, inPlaceFrequency,
                syncOutput, syncInput, inPlaceAmplitude, inPlaceFrequency,
                inPlaceComponentIndex)
                == SerializedSpectrumSyncStatus::Ok
            && inPlaceAmplitude == expectedAmplitude
            && inPlaceFrequency == expectedFrequency
            && inPlaceComponentIndex == expectedComponentIndex,
        "paired streams should support same-channel in-place output");
}

void testSerializedSpectrumSyncInputWaitsForCommonBoundary()
{
    using talas::dsp::PreparedSerializedSpectrumFrameSynchronizer;
    using talas::dsp::SerializedSpectrumSyncStatus;
    const std::array amplitudes{0.0, 0.0, 30.0, 31.0,
                                32.0, 33.0, 40.0, 41.0};
    const std::array frequencies{0.0, 0.0, 130.0, 131.0,
                                 132.0, 133.0, 140.0, 141.0};
    const std::array syncOutput{0.0, 0.0, 0.0, 0.0,
                                0.0, 0.0, 0.0, 0.0};
    const std::array syncInput{0.0, 0.0, 1.0, 1.0,
                               0.0, 0.0, 0.0, 0.0};
    const std::array expectedAmplitude{0.0, 0.0, 0.0, 0.0,
                                       30.0, 31.0, 32.0, 33.0};
    const std::array expectedFrequency{0.0, 0.0, 0.0, 0.0,
                                       130.0, 131.0, 132.0, 133.0};
    const std::array<std::size_t, 8> expectedComponentIndex{
        0, 1, 4, 4, 0, 1, 2, 3};
    PreparedSerializedSpectrumFrameSynchronizer synchronizer(4, 8);
    std::array<double, 8> amplitudeOut{};
    std::array<double, 8> frequencyOut{};
    std::array<std::size_t, 8> componentIndexOut{};
    require(synchronizer.process(amplitudes, frequencies, syncOutput, syncInput,
                amplitudeOut, frequencyOut, componentIndexOut)
                == SerializedSpectrumSyncStatus::Ok
            && amplitudeOut == expectedAmplitude
            && frequencyOut == expectedFrequency
            && componentIndexOut == expectedComponentIndex,
        "SyncInput should delay the first input component to the next common boundary");

    PreparedSerializedSpectrumFrameSynchronizer partitioned(4, 3);
    std::array<double, 8> splitAmplitude{};
    std::array<double, 8> splitFrequency{};
    std::array<std::size_t, 8> splitComponentIndex{};
    for (const auto [start, count] :
        {std::pair<std::size_t, std::size_t>{0, 3}, {3, 2}, {5, 3}})
    {
        require(partitioned.process(
                    std::span{amplitudes}.subspan(start, count),
                    std::span{frequencies}.subspan(start, count),
                    std::span{syncOutput}.subspan(start, count),
                    std::span{syncInput}.subspan(start, count),
                    std::span{splitAmplitude}.subspan(start, count),
                    std::span{splitFrequency}.subspan(start, count),
                    std::span{splitComponentIndex}.subspan(start, count))
                    == SerializedSpectrumSyncStatus::Ok,
            "partitioned SyncInput should process each bounded block");
    }
    require(splitAmplitude == amplitudeOut
            && splitFrequency == frequencyOut
            && splitComponentIndex == componentIndexOut,
        "SyncInput must preserve component phase across irregular blocks");
}

void testSerializedSpectrumSyncFailurePreservesStateAndOutputs()
{
    using talas::dsp::PreparedSerializedSpectrumFrameSynchronizer;
    using talas::dsp::SerializedSpectrumSyncStatus;
    PreparedSerializedSpectrumFrameSynchronizer synchronizer(4, 4);
    PreparedSerializedSpectrumFrameSynchronizer control(4, 4);
    const std::array warmAmplitude{10.0, 11.0};
    const std::array warmFrequency{110.0, 111.0};
    const std::array noTrigger{0.0, 0.0};
    std::array<double, 2> warmOutputA{};
    std::array<double, 2> warmOutputF{};
    std::array<std::size_t, 2> warmOutputIndex{};
    require(synchronizer.process(warmAmplitude, warmFrequency,
                noTrigger, noTrigger, warmOutputA, warmOutputF,
                warmOutputIndex)
                == SerializedSpectrumSyncStatus::Ok
            && control.process(warmAmplitude, warmFrequency,
                noTrigger, noTrigger, warmOutputA, warmOutputF,
                warmOutputIndex)
                == SerializedSpectrumSyncStatus::Ok,
        "both synchronizers should receive identical pre-trigger history");

    const std::array invalidAmplitude{
        12.0, 13.0, std::numeric_limits<double>::quiet_NaN()};
    const std::array invalidFrequency{112.0, 113.0, 120.0};
    const std::array invalidSyncOutput{1.0, 0.0, 0.0};
    const std::array invalidSyncInput{0.0, 0.0, 0.0};
    std::array<double, 3> rejectedAmplitude{7.0, 7.0, 7.0};
    std::array<double, 3> rejectedFrequency{8.0, 8.0, 8.0};
    std::array<std::size_t, 3> rejectedComponentIndex{99, 99, 99};
    require(synchronizer.process(invalidAmplitude, invalidFrequency,
                invalidSyncOutput, invalidSyncInput,
                rejectedAmplitude, rejectedFrequency,
                rejectedComponentIndex)
                == SerializedSpectrumSyncStatus::InvalidBlock
            && rejectedAmplitude == std::array{7.0, 7.0, 7.0}
            && rejectedFrequency == std::array{8.0, 8.0, 8.0}
            && rejectedComponentIndex == std::array<std::size_t, 3>{99, 99, 99},
        "late invalid spectrum data must reject the whole callback without writes");

    const std::array amplitude{12.0, 13.0, 20.0};
    const std::array frequency{112.0, 113.0, 120.0};
    std::array<double, 3> retriedAmplitude{};
    std::array<double, 3> retriedFrequency{};
    std::array<std::size_t, 3> retriedComponentIndex{};
    std::array<double, 3> controlAmplitude{};
    std::array<double, 3> controlFrequency{};
    std::array<std::size_t, 3> controlComponentIndex{};
    require(synchronizer.process(amplitude, frequency,
                invalidSyncOutput, invalidSyncInput,
                retriedAmplitude, retriedFrequency, retriedComponentIndex)
                == SerializedSpectrumSyncStatus::Ok
            && control.process(amplitude, frequency,
                invalidSyncOutput, invalidSyncInput,
                controlAmplitude, controlFrequency,
                controlComponentIndex)
                == SerializedSpectrumSyncStatus::Ok
            && retriedAmplitude == controlAmplitude
            && retriedFrequency == controlFrequency
            && retriedComponentIndex == controlComponentIndex,
        "retry after a rejected callback must see the unchanged spectral phase");

    synchronizer.reset(2);
    std::array<double, 1> outputA{7.0};
    std::array<double, 1> outputF{8.0};
    std::array<std::size_t, 1> outputIndex{99};
    const std::array oneAmplitude{20.0};
    const std::array oneFrequency{120.0};
    const std::array oneOutputTrigger{1.0};
    const std::array oneInputTrigger{0.0};
    require(synchronizer.process(oneAmplitude, oneFrequency,
                oneOutputTrigger, oneInputTrigger, outputA, outputF,
                outputIndex)
                == SerializedSpectrumSyncStatus::MissingInputFrameStart
            && outputA[0] == 7.0 && outputF[0] == 8.0
            && outputIndex[0] == 99,
        "SyncOutput must reject a trigger before component zero is in history");
    require(synchronizer.process(oneAmplitude, oneFrequency,
                oneOutputTrigger, oneOutputTrigger, outputA, outputF,
                outputIndex)
                == SerializedSpectrumSyncStatus::ConflictingTriggers
            && outputA[0] == 7.0 && outputF[0] == 8.0
            && outputIndex[0] == 99,
        "simultaneous SyncInput and SyncOutput must reject without writes");
    require(synchronizer.process(oneAmplitude, oneFrequency,
                oneInputTrigger, oneInputTrigger, outputA, outputA,
                outputIndex)
                == SerializedSpectrumSyncStatus::InvalidBlock
            && outputA[0] == 7.0 && outputIndex[0] == 99,
        "overlapping output channels must reject without writes");
}

void testSerializedSpectrumSnapshotRestoresExactComponentPhase()
{
    using talas::dsp::PreparedSerializedSpectrumFrameSynchronizer;
    using talas::dsp::SerializedSpectrumSyncSnapshot;
    using talas::dsp::SerializedSpectrumSyncStatus;
    PreparedSerializedSpectrumFrameSynchronizer synchronizer(4, 4);
    const std::array warmAmplitude{10.0, 11.0};
    const std::array warmFrequency{110.0, 111.0};
    const std::array warmControl{0.0, 0.0};
    std::array<double, 2> warmOutputAmplitude{};
    std::array<double, 2> warmOutputFrequency{};
    std::array<std::size_t, 2> warmComponentIndex{};
    require(synchronizer.process(warmAmplitude, warmFrequency,
                warmControl, warmControl,
                warmOutputAmplitude, warmOutputFrequency,
                warmComponentIndex) == SerializedSpectrumSyncStatus::Ok,
        "snapshot fixture should establish two samples of history");

    auto snapshot = synchronizer.prepareSnapshot();
    SerializedSpectrumSyncSnapshot wrongShape(5);
    require(synchronizer.capture(snapshot)
            && !synchronizer.capture(wrongShape)
            && !synchronizer.restore(wrongShape),
        "snapshot shape must match the prepared spectral period");

    const std::array amplitudes{12.0, 13.0, 20.0, 21.0};
    const std::array frequencies{112.0, 113.0, 120.0, 121.0};
    const std::array syncOutput{1.0, 1.0, 0.0, 0.0};
    const std::array syncInput{0.0, 0.0, 0.0, 0.0};
    std::array<double, 4> firstAmplitude{};
    std::array<double, 4> firstFrequency{};
    std::array<std::size_t, 4> firstComponentIndex{};
    require(synchronizer.process(amplitudes, frequencies,
                syncOutput, syncInput,
                firstAmplitude, firstFrequency,
                firstComponentIndex) == SerializedSpectrumSyncStatus::Ok
            && firstAmplitude == std::array{10.0, 11.0, 12.0, 13.0}
            && firstComponentIndex
                == std::array<std::size_t, 4>{0, 1, 2, 3},
        "the first continuation should replay a complete spectral frame");

    require(synchronizer.restore(snapshot),
        "a prepared snapshot should restore without rebuilding the DSP plan");
    std::array<double, 4> replayAmplitude{};
    std::array<double, 4> replayFrequency{};
    std::array<std::size_t, 4> replayComponentIndex{};
    require(synchronizer.process(amplitudes, frequencies,
                syncOutput, syncInput,
                replayAmplitude, replayFrequency,
                replayComponentIndex) == SerializedSpectrumSyncStatus::Ok
            && replayAmplitude == firstAmplitude
            && replayFrequency == firstFrequency
            && replayComponentIndex == firstComponentIndex,
        "restoring history, phase and edge state should reproduce exact output");

    PreparedSerializedSpectrumFrameSynchronizer pendingInput(4, 4);
    const std::array firstInputAmplitude{10.0, 20.0};
    const std::array firstInputFrequency{110.0, 120.0};
    const std::array firstInputTrigger{0.0, 1.0};
    require(pendingInput.process(firstInputAmplitude, firstInputFrequency,
                warmControl, firstInputTrigger,
                warmOutputAmplitude, warmOutputFrequency,
                warmComponentIndex) == SerializedSpectrumSyncStatus::Ok,
        "SyncInput should enter a pending alignment interval");
    auto pendingSnapshot = pendingInput.prepareSnapshot();
    require(pendingInput.capture(pendingSnapshot),
        "snapshot should capture pending startup silence");
    const std::array remainingAmplitude{21.0, 22.0, 23.0};
    const std::array remainingFrequency{121.0, 122.0, 123.0};
    const std::array remainingInputTrigger{1.0, 0.0, 0.0};
    const std::array remainingNoTrigger{0.0, 0.0, 0.0};
    std::array<double, 3> pendingAmplitude{};
    std::array<double, 3> pendingFrequency{};
    std::array<std::size_t, 3> pendingIndex{};
    require(pendingInput.process(remainingAmplitude, remainingFrequency,
                remainingNoTrigger, remainingInputTrigger,
                pendingAmplitude, pendingFrequency,
                pendingIndex) == SerializedSpectrumSyncStatus::Ok
            && pendingAmplitude == std::array{0.0, 0.0, 20.0}
            && pendingIndex == std::array<std::size_t, 3>{4, 4, 0},
        "held SyncInput should preserve its first edge and wait for the boundary");
    require(pendingInput.restore(pendingSnapshot),
        "pending SyncInput state should restore from the prepared snapshot");
    std::array<double, 3> pendingReplayAmplitude{};
    std::array<double, 3> pendingReplayFrequency{};
    std::array<std::size_t, 3> pendingReplayIndex{};
    require(pendingInput.process(remainingAmplitude, remainingFrequency,
                remainingNoTrigger, remainingInputTrigger,
                pendingReplayAmplitude, pendingReplayFrequency,
                pendingReplayIndex) == SerializedSpectrumSyncStatus::Ok
            && pendingReplayAmplitude == pendingAmplitude
            && pendingReplayFrequency == pendingFrequency
            && pendingReplayIndex == pendingIndex,
        "restoring pending silence and trigger state should reproduce the same aligned frame");
}

void testSerializedSpectrumFrameAssemblerRetainsComponentBoundaries()
{
    using talas::dsp::PreparedSerializedSpectrumFrameAssembler;
    using talas::dsp::PreparedSerializedSpectrumFrameSynchronizer;
    using talas::dsp::SerializedSpectrumAssemblyStatus;
    using talas::dsp::SerializedSpectrumSyncStatus;

    const std::array amplitudes{10.0, 11.0, 12.0, 13.0, 20.0,
                                21.0, 22.0, 23.0, 30.0, 31.0};
    const std::array frequencies{110.0, 111.0, 112.0, 113.0, 120.0,
                                 121.0, 122.0, 123.0, 130.0, 131.0};
    const std::array syncOutput{0.0, 0.0, 1.0, 0.0, 0.0,
                                0.0, 0.0, 0.0, 0.0, 0.0};
    const std::array noSync{0.0, 0.0, 0.0, 0.0, 0.0,
                            0.0, 0.0, 0.0, 0.0, 0.0};
    std::array<double, 10> alignedAmplitude{};
    std::array<double, 10> alignedFrequency{};
    std::array<std::size_t, 10> alignedIndex{};
    PreparedSerializedSpectrumFrameSynchronizer synchronizer(4, 10);
    require(synchronizer.process(amplitudes, frequencies, syncOutput, noSync,
                alignedAmplitude, alignedFrequency, alignedIndex)
                == SerializedSpectrumSyncStatus::Ok,
        "serialized spectrum fixture should synchronize before assembly");

    PreparedSerializedSpectrumFrameAssembler assembler(4, 3);
    const std::array<std::pair<std::size_t, std::size_t>, 4> blocks{{
        {0, 3}, {3, 2}, {5, 3}, {8, 2},
    }};
    for (std::size_t block = 0; block < blocks.size(); ++block)
    {
        const auto [start, count] = blocks[block];
        require(assembler.process(
                    {7, 2, start},
                    std::span{alignedAmplitude}.subspan(start, count),
                    std::span{alignedFrequency}.subspan(start, count),
                    std::span{alignedIndex}.subspan(start, count))
                    == SerializedSpectrumAssemblyStatus::Ok,
            "serialized spectral blocks should assemble in sample order");
        const auto frames = assembler.frames();
        if (block < 2)
        {
            require(frames.empty(),
                "a partial frame should not appear before its final component");
            continue;
        }
        const auto expectedStart = block == 2 ? 2U : 6U;
        const auto expectedSequence = block == 2 ? 0U : 1U;
        const auto expectedAmplitudeStart = block == 2 ? 10.0 : 20.0;
        const auto expectedFrequencyStart = block == 2 ? 110.0 : 120.0;
        require(frames.size() == 1
                && frames[0].sourceTimestamp
                    == talas::dsp::SpectralFrameTimestamp{7, 2, expectedStart}
                && frames[0].sourceSequence == expectedSequence
                && frames[0].partials.size() == 4,
            "the completed frame should retain its first-sample timestamp");
        for (std::size_t component = 0; component < 4; ++component)
        {
            const auto& partial = frames[0].partials[component];
            require(partial.hasTrackId
                    && partial.trackId == component + 1
                    && partial.amplitude
                        == expectedAmplitudeStart
                            + static_cast<double>(component)
                    && partial.frequencyHz
                        == expectedFrequencyStart
                            + static_cast<double>(component)
                    && partial.bandwidthHz == 0.0,
                "each serialized component should retain its paired values");
        }
    }
}

void testSerializedSpectrumFrameAssemblerRejectsInvalidBlockWithoutMutation()
{
    using talas::dsp::PreparedSerializedSpectrumFrameAssembler;
    using talas::dsp::SerializedSpectrumAssemblyStatus;
    PreparedSerializedSpectrumFrameAssembler assembler(4, 4);
    const std::array firstAmplitude{1.0, 2.0};
    const std::array firstFrequency{100.0, 101.0};
    const std::array<std::size_t, 2> firstIndex{0, 1};
    require(assembler.process({1, 0, 10}, firstAmplitude, firstFrequency,
                firstIndex) == SerializedSpectrumAssemblyStatus::Ok
            && assembler.frames().empty(),
        "assembler should retain a partial frame across callbacks");

    const std::array lastAmplitude{3.0, 4.0};
    const std::array invalidFrequency{102.0, -1.0};
    const std::array validFrequency{102.0, 103.0};
    const std::array<std::size_t, 2> lastIndex{2, 3};
    require(assembler.process({1, 0, 12}, lastAmplitude, invalidFrequency,
                lastIndex) == SerializedSpectrumAssemblyStatus::InvalidBlock
            && assembler.frames().empty(),
        "invalid frequencies must reject before changing partial-frame state");
    require(assembler.process({1, 0, 13}, lastAmplitude, validFrequency,
                lastIndex) == SerializedSpectrumAssemblyStatus::DiscontinuousBlock
            && assembler.frames().empty(),
        "a skipped sample must not silently complete a spectral frame");
    require(assembler.process({1, 0, 12}, lastAmplitude, validFrequency,
                lastIndex) == SerializedSpectrumAssemblyStatus::Ok
            && assembler.frames().size() == 1
            && assembler.frames()[0].sourceTimestamp.absoluteFrame == 10
            && assembler.frames()[0].partials[3].amplitude == 4.0,
        "a corrected callback should complete the original frame");
}

void testSerializedSpectrumAssemblerSnapshotRestoresPartialFrame()
{
    using talas::dsp::PreparedSerializedSpectrumFrameAssembler;
    using talas::dsp::SerializedSpectrumAssemblyStatus;
    PreparedSerializedSpectrumFrameAssembler assembler(4, 4);
    const std::array firstAmplitude{1.0, 2.0};
    const std::array firstFrequency{100.0, 101.0};
    const std::array<std::size_t, 2> firstIndex{0, 1};
    const std::array lastAmplitude{3.0, 4.0};
    const std::array lastFrequency{102.0, 103.0};
    const std::array<std::size_t, 2> lastIndex{2, 3};
    const std::array nextAmplitude{5.0, 6.0, 7.0, 8.0};
    const std::array nextFrequency{104.0, 105.0, 106.0, 107.0};
    const std::array<std::size_t, 4> nextIndex{0, 1, 2, 3};
    require(assembler.process({1, 0, 10}, firstAmplitude, firstFrequency,
                firstIndex) == SerializedSpectrumAssemblyStatus::Ok
            && assembler.frames().empty(),
        "assembler snapshot fixture should begin with a partial frame");
    auto snapshot = assembler.prepareSnapshot();
    talas::dsp::SerializedSpectrumAssemblySnapshot wrongShape(5);
    require(assembler.capture(snapshot)
            && !assembler.capture(wrongShape)
            && !assembler.restore(wrongShape),
        "assembler should capture its partial frame and reject a mismatched snapshot");
    require(assembler.process({1, 0, 12}, lastAmplitude, lastFrequency,
                lastIndex) == SerializedSpectrumAssemblyStatus::Ok
            && assembler.frames().size() == 1
            && assembler.frames()[0].sourceSequence == 0,
        "assembler should complete the captured frame");
    require(assembler.process({1, 0, 14}, nextAmplitude, nextFrequency,
                nextIndex) == SerializedSpectrumAssemblyStatus::Ok
            && assembler.frames().size() == 1
            && assembler.frames()[0].sourceSequence == 1,
        "assembler should advance its source sequence before rollback");
    require(assembler.restore(snapshot) && assembler.frames().empty(),
        "restoring assembler state should discard later output frames");
    require(assembler.process({1, 0, 12}, lastAmplitude, lastFrequency,
                lastIndex) == SerializedSpectrumAssemblyStatus::Ok
            && assembler.frames().size() == 1,
        "assembler should accept the exact retry block");
    const auto& retried = assembler.frames()[0];
    require(retried.sourceTimestamp
                == talas::dsp::SpectralFrameTimestamp{1, 0, 10}
            && retried.sourceSequence == 0
            && retried.partials.size() == 4,
        "retried frame should restore timestamp, sequence and component count");
    for (std::size_t index = 0; index < 4; ++index)
    {
        require(retried.partials[index].amplitude
                    == static_cast<double>(index + 1)
                && retried.partials[index].frequencyHz
                    == 100.0 + static_cast<double>(index),
            "retried frame should restore every staged amplitude/frequency pair");
    }
}

void testSerializedSpectrumAssemblyEventsDoNotDependOnCallbackPartition()
{
    using talas::dsp::PreparedSerializedSpectrumFrameAssembler;
    using talas::dsp::PreparedSerializedSpectrumFrameSynchronizer;
    using talas::dsp::SerializedSpectrumAssemblyStatus;
    using talas::dsp::SerializedSpectrumSyncStatus;
    const std::array amplitudes{10.0, 11.0, 12.0, 13.0,
                                20.0, 21.0, 22.0, 23.0,
                                30.0, 31.0, 32.0, 33.0};
    const std::array frequencies{110.0, 111.0, 112.0, 113.0,
                                 120.0, 121.0, 122.0, 123.0,
                                 130.0, 131.0, 132.0, 133.0};
    const std::array syncOutput{0.0, 0.0, 1.0, 0.0,
                                0.0, 0.0, 0.0, 0.0,
                                0.0, 0.0, 0.0, 0.0};
    const std::array noTrigger{0.0, 0.0, 0.0, 0.0,
                               0.0, 0.0, 0.0, 0.0,
                               0.0, 0.0, 0.0, 0.0};
    struct Record
    {
        talas::dsp::SpectralFrameTimestamp timestamp;
        std::uint64_t sequence;
        std::array<talas::dsp::PartialSpectrumPoint, 4> partials;
    };
    const auto collect = [&](std::span<const std::pair<std::size_t,
                                         std::size_t>> blocks) {
        auto maximumBlock = std::size_t{0};
        for (const auto [start, count] : blocks)
        {
            require(start + count <= amplitudes.size(),
                "partition must remain within the serialized test stream");
            maximumBlock = std::max(maximumBlock, count);
        }
        PreparedSerializedSpectrumFrameSynchronizer synchronizer(
            4, maximumBlock);
        PreparedSerializedSpectrumFrameAssembler assembler(4, maximumBlock);
        std::array<double, 12> alignedAmplitude{};
        std::array<double, 12> alignedFrequency{};
        std::array<std::size_t, 12> alignedIndex{};
        std::vector<Record> records;
        for (const auto [start, count] : blocks)
        {
            const auto amplitudeBlock = std::span{amplitudes}.subspan(
                start, count);
            const auto frequencyBlock = std::span{frequencies}.subspan(
                start, count);
            const auto outputTriggerBlock = std::span{syncOutput}.subspan(
                start, count);
            const auto noTriggerBlock = std::span{noTrigger}.subspan(
                start, count);
            const auto alignedAmplitudeBlock =
                std::span{alignedAmplitude}.subspan(start, count);
            const auto alignedFrequencyBlock =
                std::span{alignedFrequency}.subspan(start, count);
            const auto alignedIndexBlock =
                std::span{alignedIndex}.subspan(start, count);
            require(synchronizer.process(amplitudeBlock, frequencyBlock,
                        outputTriggerBlock, noTriggerBlock,
                        alignedAmplitudeBlock, alignedFrequencyBlock,
                        alignedIndexBlock) == SerializedSpectrumSyncStatus::Ok,
                "each synchronized partition should process");
            require(assembler.process({9, 0, start},
                        alignedAmplitudeBlock, alignedFrequencyBlock,
                        alignedIndexBlock)
                        == SerializedSpectrumAssemblyStatus::Ok,
                "each assembled partition should process");
            for (const auto& frame : assembler.frames())
            {
                require(frame.partials.size() == 4,
                    "each published spectral event should contain a full frame");
                Record record{frame.sourceTimestamp, frame.sourceSequence, {}};
                std::ranges::copy(frame.partials, record.partials.begin());
                records.push_back(record);
            }
        }
        return records;
    };

    const std::array wholeBlocks{
        std::pair<std::size_t, std::size_t>{0, 12}};
    const std::array<std::pair<std::size_t, std::size_t>, 5> splitBlocks{{
        {0, 3}, {3, 2}, {5, 3}, {8, 1}, {9, 3}}};
    const auto whole = collect(wholeBlocks);
    const auto split = collect(splitBlocks);
    require(whole.size() == 2 && split.size() == whole.size(),
        "both callback partitions should publish the same two complete frames");
    for (std::size_t frame = 0; frame < whole.size(); ++frame)
    {
        const auto expectedStart = frame == 0 ? 2U : 6U;
        require(whole[frame].timestamp
                    == talas::dsp::SpectralFrameTimestamp{9, 0, expectedStart}
                && whole[frame].sequence == frame
                && split[frame].timestamp == whole[frame].timestamp
                && split[frame].sequence == whole[frame].sequence,
            "frame events must retain absolute timing and order across partitions");
        for (std::size_t component = 0; component < 4; ++component)
        {
            const auto& expected = whole[frame].partials[component];
            const auto& actual = split[frame].partials[component];
            require(actual.trackId == expected.trackId
                    && actual.amplitude == expected.amplitude
                    && actual.frequencyHz == expected.frequencyHz
                    && actual.bandwidthHz == expected.bandwidthHz,
                "frame events must retain every paired component across partitions");
        }
    }
}

void testSerializedSpectrumSynchronizationChainAlignsCompletedFrame()
{
    using talas::dsp::PreparedSerializedSpectrumFrameAssembler;
    using talas::dsp::PreparedSerializedSpectrumFrameSynchronizer;
    using talas::dsp::SerializedSpectrumAssemblyStatus;
    using talas::dsp::SerializedSpectrumSyncStatus;

    const std::array amplitudes{10.0, 11.0, 12.0, 13.0,
                                20.0, 21.0, 22.0, 23.0};
    const std::array frequencies{110.0, 111.0, 112.0, 113.0,
                                 120.0, 121.0, 122.0, 123.0};
    const std::array trigger{0.0, 0.0, 1.0, 0.0,
                             0.0, 0.0, 0.0, 0.0};
    const std::array noTrigger{0.0, 0.0, 0.0, 0.0,
                               0.0, 0.0, 0.0, 0.0};
    std::array<double, 8> firstAmplitude{};
    std::array<double, 8> firstFrequency{};
    std::array<std::size_t, 8> firstIndex{};
    PreparedSerializedSpectrumFrameSynchronizer syncOutput(4, 8);
    require(syncOutput.process(amplitudes, frequencies, trigger, noTrigger,
                firstAmplitude, firstFrequency, firstIndex)
                == SerializedSpectrumSyncStatus::Ok
            && firstIndex[2] == 0,
        "SyncOutput should identify the first serialized component at recording onset");

    std::array<double, 8> alignedAmplitude{};
    std::array<double, 8> alignedFrequency{};
    std::array<std::size_t, 8> alignedIndex{};
    PreparedSerializedSpectrumFrameSynchronizer syncInput(4, 8);
    require(syncInput.process(firstAmplitude, firstFrequency,
                noTrigger, trigger,
                alignedAmplitude, alignedFrequency, alignedIndex)
                == SerializedSpectrumSyncStatus::Ok
            && alignedIndex == std::array<std::size_t, 8>{
                0, 1, 4, 4, 0, 1, 2, 3},
        "SyncInput should place the recorded first component at the shared boundary");

    PreparedSerializedSpectrumFrameAssembler assembler(4, 8);
    require(assembler.process({1, 0, 0}, alignedAmplitude,
                alignedFrequency, alignedIndex)
                == SerializedSpectrumAssemblyStatus::Ok,
        "the aligned output should assemble into a typed frame");
    const auto frames = assembler.frames();
    require(frames.size() == 1
            && frames[0].sourceTimestamp.absoluteFrame == 4
            && frames[0].partials.size() == 4,
        "the complete frame should start on the global four-sample boundary");
    for (std::size_t component = 0; component < 4; ++component)
    {
        require(frames[0].partials[component].amplitude
                    == 10.0 + static_cast<double>(component)
                && frames[0].partials[component].frequencyHz
                    == 110.0 + static_cast<double>(component),
            "the aligned frame should retain each paired component");
    }
}

void testPreparedOperationsDoNotAllocate()
{
    constexpr std::size_t size = 64;
    talas::dsp::Radix2FftPlan plan(size);
    std::array<std::complex<double>, size> values{};
    talas::dsp::PreparedPartialSpectrumSequence partialSequence(1, 1);
    talas::dsp::PreparedSerializedSpectrumFrameSynchronizer synchronizer(4, 4);
    auto synchronizerSnapshot = synchronizer.prepareSnapshot();
    talas::dsp::PreparedSerializedSpectrumFrameAssembler assembler(4, 4);
    auto assemblerSnapshot = assembler.prepareSnapshot();
    const std::array<talas::dsp::PartialSpectrumPoint, 1> partial{{
        {false, 0, 1.0, 440.0, 0.0},
    }};
    const talas::dsp::PartialSpectrumFrame frame{1, {1, 0, 0}, 0, partial};
    std::array<double, 4> encoded{};
    std::size_t written = 0;
    const std::array<double, 4> streamAmplitude{1.0, 2.0, 3.0, 4.0};
    const std::array<double, 4> streamFrequency{100.0, 101.0, 102.0, 103.0};
    const std::array<double, 4> streamSync{0.0, 0.0, 1.0, 0.0};
    const std::array<double, 4> streamNoSync{0.0, 0.0, 0.0, 0.0};
    std::array<double, 4> streamOutAmplitude{};
    std::array<double, 4> streamOutFrequency{};
    std::array<std::size_t, 4> streamOutComponentIndex{};

    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_release);
    const auto transformed = plan.forward(values) && plan.inverse(values);
    const auto appended = partialSequence.append({1, 0, 0}, 0, partial);
    const auto encodedStatus = talas::dsp::encodeLegacyPartialSpectrum(
        frame,
        encoded,
        written);
    const auto synchronized = synchronizer.process(
        streamAmplitude, streamFrequency, streamSync, streamNoSync,
        streamOutAmplitude, streamOutFrequency, streamOutComponentIndex);
    const auto captured = synchronizer.capture(synchronizerSnapshot);
    const auto restored = synchronizer.restore(synchronizerSnapshot);
    const auto assembled = assembler.process(
        {1, 0, 0}, streamOutAmplitude, streamOutFrequency,
        streamOutComponentIndex);
    const auto assemblyCaptured = assembler.capture(assemblerSnapshot);
    const auto assemblyRestored = assembler.restore(assemblerSnapshot);
    countAllocations.store(false, std::memory_order_release);

    require(transformed
                && appended == talas::dsp::SpectralFrameStatus::Ok
                && encodedStatus == talas::dsp::SpectralFrameStatus::Ok
                && synchronized
                    == talas::dsp::SerializedSpectrumSyncStatus::Ok
                && captured && restored
                && assembled
                    == talas::dsp::SerializedSpectrumAssemblyStatus::Ok
                && assemblyCaptured && assemblyRestored,
            "prepared operations should succeed in the allocation fixture");
    require(allocationCount.load(std::memory_order_relaxed) == 0,
            "FFT, frame append, compatibility encoding, spectrum synchronization and assembly must not allocate");
}
}

int main()
{
    testSupportedFftSizesAreExplicit();
    testImpulseAndSineFixTheFftConvention();
    testRoundTripPrecisionAndOutOfPlaceTransform();
    testWindowsAndColaMatrixAreDeterministic();
    testBoundedPartialFramesValidateOrderingAndCapacity();
    testBoundedComplexFramesValidateMetadataAndBins();
    testCapacityAndLegacyPartialCompatibilityAreBounded();
    testSerializedSpectrumSyncOutputStartsAtComponentZero();
    testSerializedSpectrumSyncInputWaitsForCommonBoundary();
    testSerializedSpectrumSyncFailurePreservesStateAndOutputs();
    testSerializedSpectrumSnapshotRestoresExactComponentPhase();
    testSerializedSpectrumFrameAssemblerRetainsComponentBoundaries();
    testSerializedSpectrumFrameAssemblerRejectsInvalidBlockWithoutMutation();
    testSerializedSpectrumAssemblerSnapshotRestoresPartialFrame();
    testSerializedSpectrumAssemblyEventsDoNotDependOnCallbackPartition();
    testSerializedSpectrumSynchronizationChainAlignsCompletedFrame();
    testPreparedOperationsDoNotAllocate();
    return 0;
}
