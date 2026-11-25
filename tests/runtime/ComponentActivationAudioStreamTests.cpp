#include "runtime/component_activation/ComponentActivationAudioStream.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>
#include <thread>

namespace
{
thread_local std::size_t ordinaryAllocations = 0;
thread_local std::size_t allocationFaultCountdown = std::numeric_limits<std::size_t>::max();
thread_local bool allocationFaultTriggered = false;
}

void* operator new(std::size_t size)
{
    ++ordinaryAllocations;
    if (allocationFaultCountdown != std::numeric_limits<std::size_t>::max()
        && allocationFaultCountdown-- == 0)
    {
        allocationFaultTriggered = true;
        throw std::bad_alloc{};
    }
    if (auto* memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace activation = talas::runtime::component_activation;

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
        throw std::runtime_error(message);
}

void appendU16(std::vector<std::uint8_t>& bytes, std::uint16_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
}

void appendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
    for (unsigned shift = 0; shift < 32U; shift += 8U)
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
}

std::vector<std::uint8_t> pcm16Wave(
    std::uint16_t channels,
    std::uint32_t sampleRate,
    const std::vector<std::int16_t>& interleaved)
{
    const auto dataBytes = static_cast<std::uint32_t>(
        interleaved.size() * sizeof(std::int16_t));
    std::vector<std::uint8_t> bytes;
    bytes.insert(bytes.end(), {'R', 'I', 'F', 'F'});
    appendU32(bytes, 36U + dataBytes);
    bytes.insert(bytes.end(), {'W', 'A', 'V', 'E'});
    bytes.insert(bytes.end(), {'f', 'm', 't', ' '});
    appendU32(bytes, 16U);
    appendU16(bytes, 1U);
    appendU16(bytes, channels);
    appendU32(bytes, sampleRate);
    appendU32(bytes, sampleRate * channels * 2U);
    appendU16(bytes, static_cast<std::uint16_t>(channels * 2U));
    appendU16(bytes, 16U);
    bytes.insert(bytes.end(), {'d', 'a', 't', 'a'});
    appendU32(bytes, dataBytes);
    for (const auto sample : interleaved)
        appendU16(bytes, static_cast<std::uint16_t>(sample));
    return bytes;
}

activation::ComponentActivationAudioStreamGrantV1 grant(
    std::uint16_t channels,
    std::uint32_t sampleRate,
    std::uint64_t frames,
    std::span<const std::uint8_t> bytes)
{
    activation::ComponentActivationAudioStreamGrantV1 value;
    value.header = activation::componentActivationHeaderV1(
        activation::ComponentActivationRecordKindV1::AudioStreamGrant,
        sizeof(value));
    value.sourceContentDigest =
        activation::calculateComponentActivationByteDigestV1(bytes);
    value.resourceGeneration = 7U;
    value.sourceByteCount = bytes.size();
    value.sourceFrameCount = frames;
    value.maximumSourceFrameIncrement = 64.0;
    constexpr std::string_view parameterId = "audio";
    constexpr std::string_view resourceId = "Audio/Test";
    value.parameterIdBytes = parameterId.size();
    value.resourceIdBytes = resourceId.size();
    std::copy(parameterId.begin(), parameterId.end(), value.parameterId.begin());
    std::copy(resourceId.begin(), resourceId.end(), value.resourceId.begin());
    value.sourceSampleRate = sampleRate;
    value.streamProfileVersion =
        activation::componentActivationAudioStreamProfileVersionV1;
    value.sourceEncoding = static_cast<std::uint32_t>(
        activation::ComponentActivationAudioSourceEncodingV1::Pcm16Wave);
    value.maximumReadFrames =
        activation::maximumComponentActivationAudioReadFramesV1;
    value.decodeBlockFrames =
        activation::componentActivationAudioDecodeBlockFramesV1;
    value.sourceChannelCount = channels;
    value.maximumTouchedSourceBlocks =
        activation::maximumComponentActivationAudioTouchedSourceBlocksV1;
    value.requestCapacity =
        activation::componentActivationAudioRequestCapacityV1;
    value.responseCapacity =
        activation::componentActivationAudioResponseCapacityV1;
    value.workerCacheCapacity =
        activation::componentActivationAudioWorkerCacheCapacityV1;
    value.lowWatermarkBlocks =
        activation::componentActivationAudioLowWatermarkBlocksV1;
    value.highWatermarkBlocks =
        activation::componentActivationAudioHighWatermarkBlocksV1;
    return value;
}

TalasKernelAbiImmutableAudioStreamPrepareV3 prepareDescriptor(
    const activation::ComponentActivationAudioStreamGrantV1& source,
    double initialSourceFrame = 0.0)
{
    TalasKernelAbiImmutableAudioStreamPrepareV3 value{};
    value.structSize = sizeof(value);
    value.apiVersion = TALAS_KERNEL_API_VERSION_V3;
    value.id = {source.resourceId.data(), source.resourceIdBytes};
    value.generation = source.resourceGeneration;
    std::copy(
        source.sourceContentDigest.begin(),
        source.sourceContentDigest.end(),
        value.sourceContentDigest);
    value.channelCount = source.sourceChannelCount;
    value.sampleRate = source.sourceSampleRate;
    value.frameCount = source.sourceFrameCount;
    value.initialSourceFrame = initialSourceFrame;
    value.maximumSourceFrameIncrement =
        source.maximumSourceFrameIncrement;
    value.maximumReadFrames = source.maximumReadFrames;
    return value;
}

TalasKernelAbiImmutableAudioStreamReadV3 readRequest(
    const TalasKernelAbiImmutableAudioStreamViewV3& view,
    double sourceFrame,
    double increment,
    std::uint64_t frames,
    double* output)
{
    TalasKernelAbiImmutableAudioStreamReadV3 value{};
    value.structSize = sizeof(value);
    value.apiVersion = TALAS_KERNEL_API_VERSION_V3;
    value.streamToken = view.streamToken;
    value.absoluteSourceFrame = sourceFrame;
    value.sourceFrameIncrement = increment;
    value.frameCount = frames;
    value.outputChannelCount = view.channelCount;
    value.interleavedOutput = output;
    value.outputSampleCapacity = frames * view.channelCount;
    return value;
}

void testExactMonoStereoInterpolationAndRateConversion()
{
    const auto monoBytes = pcm16Wave(
        1U, 48'000U, {0, 16'384, 32'767, 0});
    const auto monoGrant = grant(1U, 48'000U, 4U, monoBytes);
    auto mono = activation::prepareComponentActivationAudioStreamForTestV1(
        monoBytes,
        monoGrant,
        prepareDescriptor(monoGrant, 0.5),
        activation::ComponentActivationAudioStreamWorkerModeV1::ManualTest);
    require(mono.ok(), "mono component stream should prepare and prewarm");
    std::array<double, 3> monoOutput{};
    auto monoRead = readRequest(mono.stream->view(), 0.5, 0.5, 3U, monoOutput.data());
    TalasKernelAbiImmutableAudioStreamReadResultV3 monoResult{};
    require(
        mono.stream->read(monoRead, monoResult) == TALAS_KERNEL_ABI_STATUS_OK,
        "mono interpolated read should succeed");
    require(
        monoResult.outcome == TALAS_KERNEL_ABI_AUDIO_STREAM_READ_COMPLETE,
        "prewarmed mono read should be complete");
    requireNear(
        monoOutput[0], (16'384.0 / 32'767.0) * 0.5, 1.0e-12,
        "half-frame interpolation");
    requireNear(
        monoOutput[1], 16'384.0 / 32'767.0, 1.0e-12,
        "unequal-rate source progression");

    const auto stereoBytes = pcm16Wave(
        2U, 44'100U, {32'767, -32'768, 0, 16'384});
    const auto stereoGrant = grant(2U, 44'100U, 2U, stereoBytes);
    auto stereo = activation::prepareComponentActivationAudioStreamForTestV1(
        stereoBytes,
        stereoGrant,
        prepareDescriptor(stereoGrant),
        activation::ComponentActivationAudioStreamWorkerModeV1::ManualTest);
    require(stereo.ok(), "stereo component stream should prepare");
    std::array<double, 4> stereoOutput{};
    auto stereoRead = readRequest(
        stereo.stream->view(), 0.0, 1.0, 2U, stereoOutput.data());
    TalasKernelAbiImmutableAudioStreamReadResultV3 stereoResult{};
    require(
        stereo.stream->read(stereoRead, stereoResult)
            == TALAS_KERNEL_ABI_STATUS_OK,
        "stereo read should succeed");
    requireNear(stereoOutput[0], 1.0, 1.0e-12, "left channel preserved");
    requireNear(stereoOutput[1], -1.0, 1.0e-12, "right channel preserved");
    requireNear(stereoOutput[2], 0.0, 1.0e-12, "second left channel");
    requireNear(
        stereoOutput[3], 16'384.0 / 32'767.0, 1.0e-12,
        "second right channel");
}

void testIrregularPartitionsDiscontinuityEofAndHealth()
{
    std::vector<std::int16_t> samples(9'000U);
    for (std::size_t index = 0; index < samples.size(); ++index)
        samples[index] = static_cast<std::int16_t>(index % 32'768U);
    const auto bytes = pcm16Wave(1U, 48'000U, samples);
    const auto sourceGrant = grant(1U, 48'000U, samples.size(), bytes);
    auto prepared = activation::prepareComponentActivationAudioStreamForTestV1(
        bytes,
        sourceGrant,
        prepareDescriptor(sourceGrant),
        activation::ComponentActivationAudioStreamWorkerModeV1::ManualTest);
    require(prepared.ok(), "partition fixture should prewarm");

    std::array<double, 7> first{};
    auto firstRead = readRequest(
        prepared.stream->view(), 0.0, 1.0, first.size(), first.data());
    TalasKernelAbiImmutableAudioStreamReadResultV3 result{};
    require(
        prepared.stream->read(firstRead, result) == TALAS_KERNEL_ABI_STATUS_OK,
        "seven-frame partition should read");
    std::array<double, 1> second{};
    auto secondRead = readRequest(
        prepared.stream->view(), 7.0, 1.0, 1U, second.data());
    require(
        prepared.stream->read(secondRead, result) == TALAS_KERNEL_ABI_STATUS_OK,
        "one-frame continuation should read");
    const auto initialGeneration = result.seekGeneration;

    std::array<double, 1> jump{};
    auto jumpRead = readRequest(
        prepared.stream->view(), 5'000.0, 1.0, 1U, jump.data());
    require(
        prepared.stream->read(jumpRead, result) == TALAS_KERNEL_ABI_STATUS_OK,
        "a valid discontinuity should remain nonfatal");
    require(
        result.seekGeneration == initialGeneration + 1U,
        "discontinuity should advance the exact stream epoch");
    require(
        result.outcome == TALAS_KERNEL_ABI_AUDIO_STREAM_READ_UNDERRUN,
        "unprepared discontinuity should zero-fill as underrun");
    const auto afterJump = prepared.stream->health();
    require(
        afterJump.discontinuities == 1U
            && afterJump.underrunEvents == 1U
            && afterJump.missingFrames == 1U
            && afterJump.pendingRequests == 1U,
        "discontinuity underrun health should be exact");
    require(
        prepared.stream->pumpForTest() != 0U,
        "manual decoder should service the discontinuity request");

    auto recoveredRead = readRequest(
        prepared.stream->view(), 5'001.0, 1.0, 1U, jump.data());
    require(
        prepared.stream->read(recoveredRead, result)
            == TALAS_KERNEL_ABI_STATUS_OK
            && result.outcome == TALAS_KERNEL_ABI_AUDIO_STREAM_READ_COMPLETE,
        "continuous read should recover after refill");

    require(
        prepared.stream->replaceEpochAndPrewarm(
            8'999.0, std::chrono::milliseconds(50)),
        "control-thread epoch replacement should prewarm the EOF boundary");
    std::array<double, 3> eof{};
    auto eofRead = readRequest(
        prepared.stream->view(), 8'999.0, 1.0, 3U, eof.data());
    require(
        prepared.stream->read(eofRead, result) == TALAS_KERNEL_ABI_STATUS_OK
            && result.outcome == TALAS_KERNEL_ABI_AUDIO_STREAM_READ_END_OF_STREAM
            && result.endOfStreamFrameCount == 2U,
        "partial EOF should preserve the available prefix and report suffix");
    requireNear(
        eof[0], static_cast<double>(8'999U % 32'768U) / 32'767.0, 1.0e-12,
        "partial EOF available prefix");
    require(eof[1] == 0.0 && eof[2] == 0.0, "EOF suffix should zero-fill");
}

void testInvalidAndOverSpanRequestsAreFailureAtomic()
{
    std::vector<std::int16_t> samples(9'000U, 1'000);
    const auto bytes = pcm16Wave(1U, 48'000U, samples);
    const auto sourceGrant = grant(1U, 48'000U, samples.size(), bytes);
    auto prepared = activation::prepareComponentActivationAudioStreamForTestV1(
        bytes,
        sourceGrant,
        prepareDescriptor(sourceGrant),
        activation::ComponentActivationAudioStreamWorkerModeV1::ManualTest);
    require(prepared.ok(), "invalid-request fixture should prepare");
    const auto before = prepared.stream->health();
    std::array<double, 512> output{};
    output.fill(0.75);

    auto invalid = readRequest(
        prepared.stream->view(), 0.0, -1.0, 1U, output.data());
    TalasKernelAbiImmutableAudioStreamReadResultV3 result{};
    require(
        prepared.stream->read(invalid, result)
            == TALAS_KERNEL_ABI_STATUS_INVALID_VALUE,
        "reverse reads must reject");
    invalid = readRequest(
        prepared.stream->view(),
        0.0,
        64.0,
        output.size(),
        output.data());
    require(
        prepared.stream->read(invalid, result)
            == TALAS_KERNEL_ABI_STATUS_INVALID_VALUE,
        "requests touching more than sixteen blocks must reject");
    require(
        std::all_of(output.begin(), output.end(), [](double value) {
            return value == 0.75;
        }),
        "invalid requests must not modify caller output");
    const auto after = prepared.stream->health();
    require(
        after.seekGeneration == before.seekGeneration
            && after.missingFrames == before.missingFrames
            && after.discontinuities == before.discontinuities,
        "invalid requests must not change stream state or health");
}

void testQueuePressureStaleGenerationsAndContiguousUnderrunRecovery()
{
    std::vector<std::int16_t> samples(12'000U);
    for (std::size_t index = 0; index < samples.size(); ++index)
        samples[index] = static_cast<std::int16_t>(index % 32'768U);
    const auto bytes = pcm16Wave(1U, 48'000U, samples);
    const auto sourceGrant = grant(1U, 48'000U, samples.size(), bytes);
    auto prepared = activation::prepareComponentActivationAudioStreamForTestV1(
        bytes,
        sourceGrant,
        prepareDescriptor(sourceGrant),
        activation::ComponentActivationAudioStreamWorkerModeV1::ManualTest);
    require(prepared.ok(), "queue-pressure fixture should prewarm");

    std::array<double, 1> output{};
    TalasKernelAbiImmutableAudioStreamReadResultV3 result{};
    auto jump = readRequest(
        prepared.stream->view(), 5'000.0, 1.0, 1U, output.data());
    require(
        prepared.stream->read(jump, result) == TALAS_KERNEL_ABI_STATUS_OK
            && result.outcome == TALAS_KERNEL_ABI_AUDIO_STREAM_READ_UNDERRUN,
        "first unprepared generation should underrun without waiting");
    auto continuation = readRequest(
        prepared.stream->view(), 5'001.0, 1.0, 1U, output.data());
    require(
        prepared.stream->read(continuation, result)
                == TALAS_KERNEL_ABI_STATUS_OK
            && result.outcome == TALAS_KERNEL_ABI_AUDIO_STREAM_READ_UNDERRUN,
        "continuous missing data should remain a nonfatal underrun");
    auto pressureHealth = prepared.stream->health();
    require(
        pressureHealth.underrunEvents == 1U
            && pressureHealth.missingFrames == 2U
            && pressureHealth.staleBlocks
                >= activation::componentActivationAudioHighWatermarkBlocksV1,
        "one contiguous underrun and the retired prewarm generation should be counted exactly");

    double position = 1'000.0;
    for (std::size_t attempt = 0;
         attempt < activation::componentActivationAudioRequestCapacityV1;
         ++attempt)
    {
        auto request = readRequest(
            prepared.stream->view(), position, 1.0, 1U, output.data());
        require(
            prepared.stream->read(request, result) == TALAS_KERNEL_ABI_STATUS_OK,
            "valid request pressure must remain nonfatal");
        position = position == 1'000.0 ? 5'000.0 : 1'000.0;
    }
    pressureHealth = prepared.stream->health();
    require(
        pressureHealth.requestRejections != 0U
            && pressureHealth.pendingRequests
                == activation::componentActivationAudioRequestCapacityV1,
        "a full request queue should reject observably at its fixed bound");
    require(
        prepared.stream->pumpForTest() == 0U,
        "stale queued generations must not be decoded as the current epoch");
    require(
        prepared.stream->health().pendingRequests == 0U,
        "the worker should retire the bounded stale request snapshot");

    const auto currentPosition = result.expectedNextSourceFrame;
    auto retry = readRequest(
        prepared.stream->view(), currentPosition, 1.0, 1U, output.data());
    require(
        prepared.stream->read(retry, result) == TALAS_KERNEL_ABI_STATUS_OK
            && result.outcome == TALAS_KERNEL_ABI_AUDIO_STREAM_READ_UNDERRUN,
        "the callback after stale retirement should republish without waiting");
    require(
        prepared.stream->pumpForTest() != 0U,
        "the current generation should refill after bounded pressure clears");
    auto recovered = readRequest(
        prepared.stream->view(), result.expectedNextSourceFrame, 1.0, 1U,
        output.data());
    require(
        prepared.stream->read(recovered, result) == TALAS_KERNEL_ABI_STATUS_OK
            && result.outcome == TALAS_KERNEL_ABI_AUDIO_STREAM_READ_COMPLETE,
        "streaming should recover after request pressure without a new epoch");
}

void testShutdownClosesReadsAndCountersSaturate()
{
    const auto bytes = pcm16Wave(1U, 48'000U, {0, 1, 2, 3});
    const auto sourceGrant = grant(1U, 48'000U, 4U, bytes);
    auto prepared = activation::prepareComponentActivationAudioStreamForTestV1(
        bytes,
        sourceGrant,
        prepareDescriptor(sourceGrant),
        activation::ComponentActivationAudioStreamWorkerModeV1::ManualTest);
    require(prepared.ok(), "shutdown fixture should prewarm");

    prepared.stream->setHealthCountersForTest(
        std::numeric_limits<std::uint64_t>::max() - 1U);
    std::array<double, 3> output{};
    auto eof = readRequest(
        prepared.stream->view(), 3.0, 1.0, output.size(), output.data());
    TalasKernelAbiImmutableAudioStreamReadResultV3 result{};
    require(
        prepared.stream->read(eof, result) == TALAS_KERNEL_ABI_STATUS_OK,
        "saturation fixture read should remain valid");
    const auto saturated = prepared.stream->health();
    require(
        saturated.missingFrames == std::numeric_limits<std::uint64_t>::max()
            && saturated.endOfStreamFrames
                == std::numeric_limits<std::uint64_t>::max(),
        "health counters must saturate instead of wrapping");

    prepared.stream->shutdown();
    output.fill(0.75);
    auto closed = readRequest(
        prepared.stream->view(), 0.0, 1.0, 1U, output.data());
    require(
        prepared.stream->read(closed, result)
            == TALAS_KERNEL_ABI_STATUS_QUARANTINED,
        "reads after shutdown must fail closed");
    require(output[0] == 0.75, "a closed read must not modify output");
}
}

namespace
{
double decodedSample(std::int16_t sample)
{
    return sample == -32768 ? -1.0 : static_cast<double>(sample) / 32767.0;
}

void expectPcm(std::span<const double> output,
    const std::vector<std::int16_t>& samples, std::size_t channels,
    double start, double increment)
{
    const auto frames = samples.size() / channels;
    for (std::size_t i = 0; i < output.size() / channels; ++i)
    {
        const double position = start + static_cast<double>(i) * increment;
        for (std::size_t channel = 0; channel < channels; ++channel)
        {
            double expected = 0;
            if (position < static_cast<double>(frames))
            {
                const auto left = static_cast<std::size_t>(position);
                const auto right = std::min(left + 1, frames - 1);
                const auto a = decodedSample(samples[left * channels + channel]);
                const auto b = decodedSample(samples[right * channels + channel]);
                expected = a + (b - a) * (position - static_cast<double>(left));
            }
            requireNear(output[i * channels + channel], expected, 1e-12,
                "restored stream must follow independently decoded PCM history");
        }
    }
}

template<class Stream>
void testOfflineCheckpoint()
{
    if constexpr (!requires(Stream& stream) {
        stream.beginOfflineCheckpoint(false);
        stream.restoreOfflineCheckpoint();
        stream.commitOfflineCheckpoint();
    })
    {
        require(false, "component resource stream lacks processing checkpoint API");
    }
    else
    {
        std::vector<std::int16_t> samples(28000);
        for (std::size_t i = 0; i < samples.size(); ++i)
            samples[i] = static_cast<std::int16_t>(static_cast<int>((i * 37) % 60001) - 30000);
        const auto bytes = pcm16Wave(2, 48000, samples);
        const auto sourceGrant = grant(2, 48000, samples.size() / 2, bytes);
        for (bool deferred : {false, true})
        for (double prefix : {0.0, 7.0})
        {
            auto prepared = activation::prepareComponentActivationAudioStreamForTestV1(
                bytes, sourceGrant, prepareDescriptor(sourceGrant),
                activation::ComponentActivationAudioStreamWorkerModeV1::ManualTest);
            require(prepared.ok(), "checkpoint PCM fixture should prewarm");
            Stream& stream = *prepared.stream;
            require(stream.bindResourceSetSlot(1), "checkpoint fixture slot should bind");
            const auto* view = &stream.view();
            const auto* id = view->id.data;
            const auto token = view->streamToken;
            std::array<double, 34> output{};
            TalasKernelAbiImmutableAudioStreamReadResultV3 result{};
            if (prefix != 0)
                require(stream.read(readRequest(stream.view(), 0, 1, 7, output.data()), result)
                    == TALAS_KERNEL_ABI_STATUS_OK, "nonzero checkpoint prefix should read");
            for (std::size_t attempt = 0; attempt < 40; ++attempt)
            {
                const auto baseline = stream.health();
                require(stream.beginOfflineCheckpoint(deferred), "stream checkpoint should capture");
                require(!stream.beginOfflineCheckpoint(deferred), "nested capture must retain first baseline");
                require(stream.read(readRequest(stream.view(), prefix, 0.5, 17, output.data()), result)
                    == TALAS_KERNEL_ABI_STATUS_OK, "provisional fractional read should succeed");
                expectPcm(output, samples, 2, prefix, 0.5);
                require(stream.replaceEpochAndPrewarm(12000, std::chrono::milliseconds(50)),
                    "provisional epoch replacement should prewarm");
                require(stream.replaceEpochAndPrewarm(7000, std::chrono::milliseconds(50))
                    && stream.replaceEpochAndPrewarm(10000, std::chrono::milliseconds(50)),
                    "provisional refills must wrap and overwrite the captured response slots");
                require(stream.read(readRequest(stream.view(), 13999, 1, 3, output.data()), result)
                    == TALAS_KERNEL_ABI_STATUS_OK, "provisional EOF should read");
                require(result.endOfStreamFrameCount == 2, "provisional EOF suffix should be explicit");
                if (deferred) require(stream.health() == baseline, "provisional health must remain deferred");
                else require(stream.health() != baseline, "ordinary health should expose provisional work");
                const auto before = ordinaryAllocations;
                const auto restored = stream.restoreOfflineCheckpoint();
                const auto after = ordinaryAllocations;
                require(restored && before == after, "stream restore must allocate no memory");
                require(stream.health() == baseline, "stream restore must recover all health and queue counts");
                require(&stream.view() == view && stream.view().id.data == id && stream.view().streamToken == token,
                    "stream restore must preserve prepared view and token backing");
                require(stream.read(readRequest(stream.view(), prefix, 0.5, 17, output.data()), result)
                    == TALAS_KERNEL_ABI_STATUS_OK && result.outcome == TALAS_KERNEL_ABI_AUDIO_STREAM_READ_COMPLETE
                    && result.seekGeneration == baseline.seekGeneration,
                    "restored continuation must not seek or underrun");
                expectPcm(output, samples, 2, prefix, 0.5);
                prefix += 8.5;
            }
            require(stream.beginOfflineCheckpoint(deferred), "commit baseline should capture");
            require(stream.read(readRequest(stream.view(), prefix, 0.5, 17, output.data()), result)
                == TALAS_KERNEL_ABI_STATUS_OK, "committed prefix should read");
            const auto before = ordinaryAllocations;
            const auto committed = stream.commitOfflineCheckpoint();
            const auto after = ordinaryAllocations;
            require(committed && before == after, "commit must allocate no memory");
            require(!stream.restoreOfflineCheckpoint() && !stream.commitOfflineCheckpoint(),
                "consumed checkpoint cannot be reused");
            prefix += 8.5;
            require(stream.read(readRequest(stream.view(), prefix, 0.5, 17, output.data()), result)
                == TALAS_KERNEL_ABI_STATUS_OK && result.outcome == TALAS_KERNEL_ABI_AUDIO_STREAM_READ_COMPLETE,
                "committed history should continue without discontinuity");
            expectPcm(output, samples, 2, prefix, 0.5);
        }
    }
}

void testPendingCheckpointAndCacheHistory()
{
    std::vector<std::int16_t> samples(60000);
    for (std::size_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<std::int16_t>((i * 13) % 32768);
    const auto bytes = pcm16Wave(1, 48000, samples);
    const auto sourceGrant = grant(1, 48000, samples.size(), bytes);
    auto prepared = activation::prepareComponentActivationAudioStreamForTestV1(
        bytes, sourceGrant, prepareDescriptor(sourceGrant),
        activation::ComponentActivationAudioStreamWorkerModeV1::ManualTest);
    require(prepared.ok(), "pending checkpoint fixture should prepare");
    auto& stream = *prepared.stream;
    std::array<double, 1> output{};
    TalasKernelAbiImmutableAudioStreamReadResultV3 result{};
    require(stream.beginOfflineCheckpoint(), "cache baseline should capture");
    for (double position : {10000., 20000., 30000., 40000., 50000.})
        require(stream.replaceEpochAndPrewarm(position, std::chrono::milliseconds(50)),
            "provisional cache eviction should prewarm");
    require(stream.restoreOfflineCheckpoint(), "cache history should restore");
    const auto decoded = stream.health().decodedBlocks;
    require(stream.replaceEpochAndPrewarm(0, std::chrono::milliseconds(50))
        && stream.health().decodedBlocks == decoded,
        "restored decoded cache must service an epoch change without decoding again");

    require(stream.read(readRequest(stream.view(), 5000, 1, 1, output.data()), result)
        == TALAS_KERNEL_ABI_STATUS_OK && result.outcome == TALAS_KERNEL_ABI_AUDIO_STREAM_READ_UNDERRUN,
        "pending baseline should have an unserviced discontinuity");
    const auto baseline = stream.health();
    require(baseline.pendingRequests == 1 && baseline.underrunEvents == 1,
        "pending baseline queue and underrun episode should be explicit");
    for (bool deferred : {false, true})
    {
        require(stream.beginOfflineCheckpoint(deferred), "pending request should capture");
        require(stream.pumpForTest() == activation::componentActivationAudioHighWatermarkBlocksV1,
            "pending request should decode its exact bounded window");
        require(stream.read(readRequest(stream.view(), 5001, 1, 1, output.data()), result)
            == TALAS_KERNEL_ABI_STATUS_OK && result.outcome == TALAS_KERNEL_ABI_AUDIO_STREAM_READ_COMPLETE,
            "provisional pending response should supply PCM");
        expectPcm(output, samples, 1, 5001, 1);
        require(stream.restoreOfflineCheckpoint() && stream.health() == baseline,
            "restore must recover pending request payload and health");
    }
    require(stream.read(readRequest(stream.view(), 5001, 1, 1, output.data()), result)
        == TALAS_KERNEL_ABI_STATUS_OK && result.outcome == TALAS_KERNEL_ABI_AUDIO_STREAM_READ_UNDERRUN
        && stream.health().underrunEvents == 1,
        "restored pending history must retain its single contiguous underrun episode");
    require(stream.pumpForTest() != 0, "restored pending request must be serviceable");
    require(stream.read(readRequest(stream.view(), 5002, 1, 1, output.data()), result)
        == TALAS_KERNEL_ABI_STATUS_OK && result.outcome == TALAS_KERNEL_ABI_AUDIO_STREAM_READ_COMPLETE,
        "restored request payload must refill the correct source window");
    expectPcm(output, samples, 1, 5002, 1);

    for (std::size_t i = 0; i <= activation::componentActivationAudioRequestCapacityV1; ++i)
        require(stream.read(readRequest(stream.view(), i % 2 ? 1000 : 9000, 1, 1, output.data()), result)
            == TALAS_KERNEL_ABI_STATUS_OK, "stale queue setup should remain nonfatal");
    const auto full = stream.health();
    require(full.pendingRequests == activation::componentActivationAudioRequestCapacityV1
        && full.requestRejections != 0, "checkpoint should include a full stale request queue");
    require(stream.beginOfflineCheckpoint(true), "full stale queue should capture");
    require(stream.pumpForTest() == 0 && stream.health() == full,
        "provisional stale retirement should remain deferred");
    require(stream.replaceEpochAndPrewarm(20000, std::chrono::milliseconds(50)),
        "provisional emptied queue should accept new requests");
    require(stream.restoreOfflineCheckpoint() && stream.health() == full,
        "full stale queue should restore exactly");
    require(stream.pumpForTest() == 0 && stream.health().pendingRequests == 0,
        "restored stale requests must retire without decoding into the current epoch");
}

void testCheckpointAllocationAndTerminalAuthority()
{
    const auto bytes = pcm16Wave(1, 48000, {0, 16384, 32767, -32768});
    const auto sourceGrant = grant(1, 48000, 4, bytes);
    for (bool deferred : {false, true})
    {
        auto prepared = activation::prepareComponentActivationAudioStreamForTestV1(
            bytes, sourceGrant, prepareDescriptor(sourceGrant),
            activation::ComponentActivationAudioStreamWorkerModeV1::ManualTest);
        require(prepared.ok(), "allocation fixture should prepare");
        auto& stream = *prepared.stream;
        const auto baseline = stream.health();
        const auto before = ordinaryAllocations;
        const auto captured = stream.beginOfflineCheckpoint(deferred);
        const auto count = ordinaryAllocations - before;
        require(captured && count != 0, "capture should expose its ordinary allocation sites");
        require(stream.commitOfflineCheckpoint(), "measurement checkpoint should commit");
        for (std::size_t site = 0; site < count; ++site)
        {
            allocationFaultTriggered = false;
            allocationFaultCountdown = site;
            const auto accepted = stream.beginOfflineCheckpoint(deferred);
            allocationFaultCountdown = std::numeric_limits<std::size_t>::max();
            require(!accepted && allocationFaultTriggered && stream.health() == baseline,
                "failed checkpoint allocation must preserve live stream state and health");
            require(stream.beginOfflineCheckpoint(deferred), "failed capture must permit retry");
            std::array<double, 3> output{};
            TalasKernelAbiImmutableAudioStreamReadResultV3 result{};
            require(stream.read(readRequest(stream.view(), 0, 0.5, 3, output.data()), result)
                == TALAS_KERNEL_ABI_STATUS_OK && result.outcome == TALAS_KERNEL_ABI_AUDIO_STREAM_READ_COMPLETE,
                "retry must retain prewarmed fractional audio");
            requireNear(output[1], (16384.0 / 32767.0) * 0.5, 1e-12, "capture allocation retry PCM");
            require(stream.restoreOfflineCheckpoint(), "capture allocation retry should restore");
        }
        std::cout << "Component stream checkpoint allocation sites (deferred=" << deferred << "): " << count << '\n';
        require(stream.replaceEpochAndPrewarm(3, std::chrono::milliseconds(50)), "EOF baseline should prewarm");
        require(stream.beginOfflineCheckpoint(deferred), "EOF baseline should capture");
        std::array<double, 3> output{};
        TalasKernelAbiImmutableAudioStreamReadResultV3 result{};
        require(stream.read(readRequest(stream.view(), 3, 1, 3, output.data()), result)
            == TALAS_KERNEL_ABI_STATUS_OK && result.endOfStreamFrameCount == 2,
            "EOF provisional read should report exact suffix");
        require(stream.restoreOfflineCheckpoint(), "EOF checkpoint should restore");
        require(stream.read(readRequest(stream.view(), 3, 1, 3, output.data()), result)
            == TALAS_KERNEL_ABI_STATUS_OK && result.endOfStreamFrameCount == 2 && output[0] == -1
            && output[1] == 0 && output[2] == 0, "EOF retry should preserve exact PCM and zero suffix");
        require(stream.beginOfflineCheckpoint(deferred), "fatal fixture should capture");
        stream.failWorkerForTest();
        const auto failed = stream.health();
        require(failed.workerState == activation::ComponentActivationAudioStreamWorkerStateV1::Failed
            && failed.decodeFailures == 1, "deferred inspection must expose terminal worker failure");
        require(!stream.restoreOfflineCheckpoint() && !stream.beginOfflineCheckpoint(deferred)
            && stream.health() == failed && stream.pumpForTest() == 0,
            "restore or worker pump must never revive a failed decoder");
        output.fill(0.75);
        require(stream.read(readRequest(stream.view(), 0, 1, 1, output.data()), result)
            == TALAS_KERNEL_ABI_STATUS_QUARANTINED && output[0] == 0.75,
            "failed decoder must retain rejection and caller output");
    }
    for (bool explicitShutdown : {false, true})
    {
        auto prepared = activation::prepareComponentActivationAudioStreamForTestV1(
            bytes, sourceGrant, prepareDescriptor(sourceGrant),
            activation::ComponentActivationAudioStreamWorkerModeV1::Background);
        require(prepared.ok() && prepared.stream->beginOfflineCheckpoint(true), "shutdown fixture should capture");
        if (explicitShutdown)
        {
            prepared.stream->shutdown();
            require(!prepared.stream->restoreOfflineCheckpoint()
                && !prepared.stream->beginOfflineCheckpoint(true)
                && prepared.stream->health().workerState == activation::ComponentActivationAudioStreamWorkerStateV1::Stopped,
                "shutdown must discard baseline and retain terminal authority");
        }
    }
}

void testBackgroundCheckpointWithHealthReader()
{
    std::vector<std::int16_t> samples(14000);
    for (std::size_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<std::int16_t>((i * 19) % 32768);
    const auto bytes = pcm16Wave(1, 48000, samples);
    const auto sourceGrant = grant(1, 48000, samples.size(), bytes);
    auto prepared = activation::prepareComponentActivationAudioStreamForTestV1(
        bytes, sourceGrant, prepareDescriptor(sourceGrant),
        activation::ComponentActivationAudioStreamWorkerModeV1::Background);
    require(prepared.ok(), "background checkpoint fixture should prewarm");
    auto& stream = *prepared.stream;
    std::atomic<bool> stop{false};
    std::atomic<std::size_t> inspections{0};
    std::atomic<bool> invalid{false};
    std::thread reader([&] {
        while (!stop.load(std::memory_order_acquire))
        {
            const auto health = stream.health();
            if (health.bufferedBlocks > activation::componentActivationAudioResponseCapacityV1
                || health.pendingRequests > activation::componentActivationAudioRequestCapacityV1
                || health.seekGeneration == 0) invalid.store(true);
            ++inspections;
            std::this_thread::yield();
        }
    });
    // Always join the reader, including on an assertion failure.
    struct Join
    {
        std::atomic<bool>& stop;
        std::thread& reader;
        ~Join() { stop.store(true, std::memory_order_release); reader.join(); }
    } join{stop, reader};
    double prefix = 0;
    for (std::size_t attempt = 0; attempt < 60; ++attempt)
    {
        const bool deferred = attempt % 2 == 0;
        require(stream.beginOfflineCheckpoint(deferred), "background worker checkpoint should capture");
        const auto baseline = stream.health();
        require(stream.replaceEpochAndPrewarm(10000, std::chrono::milliseconds(250)),
            "background worker should process provisional refill");
        if (deferred)
            require(stream.health() == baseline, "background provisional health must remain at captured boundary");
        require(stream.restoreOfflineCheckpoint(), "background worker checkpoint should restore");
        std::array<double, 7> output{};
        TalasKernelAbiImmutableAudioStreamReadResultV3 result{};
        const auto before = ordinaryAllocations;
        const auto status = stream.read(readRequest(stream.view(), prefix, 0.5, 7, output.data()), result);
        const auto after = ordinaryAllocations;
        require(status == TALAS_KERNEL_ABI_STATUS_OK && result.outcome == TALAS_KERNEL_ABI_AUDIO_STREAM_READ_COMPLETE
            && result.seekGeneration == baseline.seekGeneration && before == after,
            "background restore must preserve allocation-free continuous audio");
        expectPcm(output, samples, 1, prefix, 0.5);
        prefix += 3.5;
    }
    require(inspections.load() != 0 && !invalid.load(), "concurrent health must retain bounded queue snapshots");
}
}

int main()
{
    testExactMonoStereoInterpolationAndRateConversion();
    testIrregularPartitionsDiscontinuityEofAndHealth();
    testInvalidAndOverSpanRequestsAreFailureAtomic();
    testQueuePressureStaleGenerationsAndContiguousUnderrunRecovery();
    testShutdownClosesReadsAndCountersSaturate();
    testOfflineCheckpoint<activation::ComponentActivationAudioStreamV1>();
    testPendingCheckpointAndCacheHistory();
    testCheckpointAllocationAndTerminalAuthority();
    testBackgroundCheckpointWithHealthReader();
    return 0;
}
