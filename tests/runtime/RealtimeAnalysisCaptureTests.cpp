#include "talas/runtime/RealtimeAnalysisCapture.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
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

void operator delete(void* memory) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory) noexcept
{
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept
{
    std::free(memory);
}

namespace
{
using talas::runtime::RealtimeAnalysisCapture;
using talas::runtime::RealtimeAnalysisCopyStatus;
using talas::runtime::RealtimeAnalysisKind;
using talas::runtime::RealtimeAnalysisProbeSpec;
using talas::runtime::RealtimeAnalysisPayloadKind;

static_assert(noexcept(std::declval<RealtimeAnalysisCapture&>().publish(
    0, std::declval<std::span<const double>>())));
static_assert(noexcept(std::declval<RealtimeAnalysisCapture&>().copyLatest(
    0, std::declval<std::span<double>>())));
static_assert(noexcept(std::declval<RealtimeAnalysisCapture&>().publishRecord(
    0,
    0,
    RealtimeAnalysisPayloadKind::AudioBlock,
    1,
    std::declval<std::span<const double>>())));
static_assert(noexcept(
    std::declval<RealtimeAnalysisCapture&>().copyLatestRecord(
        0, std::declval<std::span<double>>())));
static_assert(noexcept(
    std::declval<RealtimeAnalysisCapture&>().copyLatestRecordResult(
        0, std::declval<std::span<double>>())));
static_assert(noexcept(
    std::declval<RealtimeAnalysisCapture&>().rejectPreparedRecord(0)));
static_assert(noexcept(
    std::declval<RealtimeAnalysisCapture&>().publishPartialSpectrum(
        0,
        0,
        std::declval<std::span<const talas::dsp::PartialSpectrumPoint>>())));
static_assert(noexcept(
    std::declval<RealtimeAnalysisCapture&>().copyLatestPartialSpectrum(
        0,
        std::declval<std::span<talas::dsp::PartialSpectrumPoint>>())));

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void requireNear(double actual, double expected, const std::string& message)
{
    if (std::abs(actual - expected) > 1.0e-12)
        throw std::runtime_error(message);
}

void testCaptureRejectsInvalidPreparation()
{
    RealtimeAnalysisCapture empty({}, 4, 2);
    require(empty.ready(), "capture with no probes should be a valid disabled capture");

    RealtimeAnalysisCapture zeroFrames(
        {{"scope", 0, RealtimeAnalysisKind::Oscilloscope}}, 0, 2);
    require(!zeroFrames.ready(), "capture should reject zero maximum frames");

    RealtimeAnalysisCapture zeroCapacity(
        {{"scope", 0, RealtimeAnalysisKind::Oscilloscope}}, 4, 0);
    require(!zeroCapacity.ready(), "capture should reject zero queue capacity");

    RealtimeAnalysisCapture emptyNode(
        {{"", 0, RealtimeAnalysisKind::Oscilloscope}}, 4, 2);
    require(!emptyNode.ready(), "capture should reject an empty node ID");

    RealtimeAnalysisCapture duplicate(
        {
            {"scope", 3, RealtimeAnalysisKind::Oscilloscope},
            {"scope", 3, RealtimeAnalysisKind::Spectrum},
        },
        4,
        2);
    require(!duplicate.ready(), "capture should reject duplicate node/scope keys");

    RealtimeAnalysisCapture duplicateClient(
        {
            {"scope-a", 1, RealtimeAnalysisKind::Oscilloscope,
             RealtimeAnalysisPayloadKind::AudioBlock, 1, "audioOut", "trace-a"},
            {"scope-b", 2, RealtimeAnalysisKind::Oscilloscope,
             RealtimeAnalysisPayloadKind::AudioBlock, 1, "audioOut", "trace-a"},
        },
        4,
        2);
    require(!duplicateClient.ready(),
        "capture should reject duplicate non-empty client identities");

    RealtimeAnalysisCapture tooManyProbes(
        std::vector<RealtimeAnalysisProbeSpec>(
            talas::runtime::maximumRealtimeAnalysisProbeCount + 1,
            {"scope", 0, RealtimeAnalysisKind::Oscilloscope}),
        4,
        2);
    require(!tooManyProbes.ready(), "capture should reject too many probes");

    RealtimeAnalysisCapture tooManyValues(
        {{"scope", 0, RealtimeAnalysisKind::Oscilloscope}},
        talas::runtime::maximumRealtimeAnalysisPayloadValues + 1,
        2);
    require(!tooManyValues.ready(), "capture should reject oversized payload storage");

    RealtimeAnalysisCapture tooDeep(
        {{"scope", 0, RealtimeAnalysisKind::Oscilloscope}},
        4,
        talas::runtime::maximumRealtimeAnalysisQueueCapacity + 1);
    require(!tooDeep.ready(), "capture should reject an oversized queue");

    RealtimeAnalysisCapture zeroChannels(
        {{"scope", 0, RealtimeAnalysisKind::Oscilloscope,
          RealtimeAnalysisPayloadKind::AudioBlock, 0}},
        4,
        2);
    require(!zeroChannels.ready(), "capture should reject zero payload channels");

    RealtimeAnalysisCapture overlongHistory(
        {{"scope", 0, RealtimeAnalysisKind::Oscilloscope,
          RealtimeAnalysisPayloadKind::AudioBlock, 1, "audioOut", "", 8}},
        4, 2);
    require(!overlongHistory.ready(),
        "rolling history must fit the prepared bounded payload capacity");
}

void testCaptureFindsDeterministicProbeIdentity()
{
    RealtimeAnalysisCapture capture(
        {
            {"scope", 2, RealtimeAnalysisKind::Oscilloscope,
             RealtimeAnalysisPayloadKind::AudioBlock, 1, "audioOut", "trace-b"},
            {"scope", 1, RealtimeAnalysisKind::Oscilloscope,
             RealtimeAnalysisPayloadKind::AudioBlock, 1, "audioOut", "trace-a"},
            {"spectrum", 0, RealtimeAnalysisKind::Spectrum,
             RealtimeAnalysisPayloadKind::AudioBlock, 1, "audioOut", "trace-spectrum"},
        },
        4,
        2);
    require(capture.ready(), "valid capture should prepare");
    require(capture.probeCount() == 3, "capture should preserve every probe");
    require(capture.probeSpec(0) != nullptr
                && capture.probeSpec(0)->nodeId == "scope"
                && capture.probeSpec(0)->scopeId == 2,
            "capture should preserve immutable probe metadata");
    require(capture.probeIndex("scope", 2) == std::optional<std::size_t>{0},
            "exact node/scope lookup should resolve the requested probe");
    require(capture.preferredProbeIndex("scope") == std::optional<std::size_t>{1},
            "saved-node lookup should choose the lowest prepared scope");
    require(capture.preferredProbeIndex("spectrum") == std::optional<std::size_t>{2},
            "saved-node lookup should prefer the global scope");
    require(capture.probeIndexForClientId("trace-a")
                == std::optional<std::size_t>{1},
            "stable client identity should resolve the exact prepared probe");
    require(!capture.probeIndexForClientId({}).has_value()
                && !capture.probeIndexForClientId("missing").has_value(),
            "empty or unknown client identity should not resolve");
    require(!capture.probeIndex("missing", 0).has_value(), "unknown probe should not resolve");
    require(capture.probeSpec(3) == nullptr, "out-of-range probe metadata should reject");
}

void testCapturePublishesAndDrainsNewestCompleteBlock()
{
    RealtimeAnalysisCapture capture(
        {{"scope", 0, RealtimeAnalysisKind::Oscilloscope}},
        4,
        3);
    const std::array first{1.0, 2.0};
    const std::array second{3.0, 4.0, 5.0};
    require(capture.publish(0, first), "first block should publish");
    require(capture.publish(0, second), "second block should publish");

    std::array<double, 4> output{};
    const auto copied = capture.copyLatest(0, output);
    require(copied == std::optional<std::size_t>{3}, "consumer should receive newest block length");
    for (std::size_t index = 0; index < second.size(); ++index)
        requireNear(output[index], second[index], "consumer should receive exact newest samples");
    require(!capture.copyLatest(0, output).has_value(), "drained capture should be empty");
}

void testCaptureDropsWhenFullAndRetainsBlockForLargeEnoughDestination()
{
    RealtimeAnalysisCapture capture(
        {{"scope", 0, RealtimeAnalysisKind::Oscilloscope}},
        4,
        2);
    const std::array first{1.0};
    const std::array second{2.0, 3.0, 4.0};
    const std::array third{5.0};
    require(capture.publish(0, first), "first bounded block should publish");
    require(capture.publish(0, second), "second bounded block should publish");
    require(!capture.publish(0, third), "full queue should drop newest block");
    require(capture.droppedBlockCount(0) == 1, "full queue should account for dropped block");

    std::array<double, 2> tooSmall{};
    require(!capture.copyLatest(0, tooSmall).has_value(),
            "undersized destination should reject without consuming newest block");
    std::array<double, 4> output{};
    require(capture.copyLatest(0, output) == std::optional<std::size_t>{3},
            "large destination should still receive retained newest block");
    requireNear(output[0], 2.0, "retained newest block sample zero");
    requireNear(output[2], 4.0, "retained newest block sample two");
}

void testRollingOscilloscopeHistorySurvivesSkippedCallbacksAndResets()
{
    RealtimeAnalysisCapture capture(
        {{"scope", 0, RealtimeAnalysisKind::Oscilloscope,
          RealtimeAnalysisPayloadKind::AudioBlock, 1, "audioOut", "", 8}},
        8, 1);
    require(capture.ready() && capture.bindStage(77),
        "bounded rolling oscilloscope probe should prepare and bind");
    const auto publish = [&](std::uint64_t frame, std::span<const double> values) {
        return capture.publishRecord(0, frame,
            RealtimeAnalysisPayloadKind::AudioBlock, 1, values);
    };
    const std::array first{1.0, 2.0, 3.0};
    const std::array second{4.0, 5.0};
    const std::array third{6.0, 7.0, 8.0, 9.0};
    std::array<double, 8> output{};
    require(publish(0, first), "first rolling callback should publish");
    auto record = capture.copyLatestRecord(0, output);
    require(record.has_value() && record->valueCount == 3
            && record->absoluteSamplePosition == 0,
        "first rolling record should contain only observed samples");

    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_release);
    const auto publishedSecond = publish(3, second);
    record = capture.copyLatestRecord(0, output);
    const auto publishedThird = publish(5, third);
    const auto thirdRecord = capture.copyLatestRecord(0, output);
    countAllocations.store(false, std::memory_order_release);
    require(publishedSecond && publishedThird
            && thirdRecord.has_value()
            && thirdRecord->valueCount == 8
            && thirdRecord->absoluteSamplePosition == 1
            && output == std::array<double, 8>{2, 3, 4, 5, 6, 7, 8, 9},
        "rolling probe should retain the newest contiguous eight samples");
    require(allocationCount.load(std::memory_order_relaxed) == 0,
        "rolling publication and consumption should not allocate");

    const std::array fourth{10.0};
    const std::array skipped{11.0};
    const std::array sixth{12.0};
    require(publish(9, fourth), "queued rolling record should publish");
    require(!publish(10, skipped)
            && capture.droppedBlockCount(0) == 1,
        "full queue should drop a record while retaining its signal history");
    require(capture.copyLatestRecord(0, output).has_value(),
        "consumer should drain the prior queued snapshot");
    require(publish(11, sixth),
        "next rolling record should publish after the queue drains");
    record = capture.copyLatestRecord(0, output);
    require(record.has_value()
            && record->absoluteSamplePosition == 4
            && record->valueCount == 8
            && output == std::array<double, 8>{5, 6, 7, 8, 9, 10, 11, 12},
        "a skipped UI callback must remain in the next rolling snapshot");

    require(capture.bindStage(78), "stage replacement should rebind capture");
    const std::array restaged{20.0};
    require(publish(100, restaged), "new stage should publish");
    record = capture.copyLatestRecord(0, output);
    require(record.has_value() && record->valueCount == 1
            && record->absoluteSamplePosition == 100
            && output[0] == 20.0,
        "new stage must not expose prior rolling audio");
    const std::array discontinuous{21.0};
    require(publish(200, discontinuous),
        "transport discontinuity should publish a new bounded snapshot");
    record = capture.copyLatestRecord(0, output);
    require(record.has_value() && record->valueCount == 1
            && record->absoluteSamplePosition == 200
            && output[0] == 21.0,
        "noncontiguous callbacks must reset rolling history");
}

void testDecimatedSweepRetainsSlowHistoryAcrossShortCallbacks()
{
    RealtimeAnalysisProbeSpec spec{
        "scope", 0, RealtimeAnalysisKind::Oscilloscope,
        RealtimeAnalysisPayloadKind::AudioBlock, 1, "@slow-sweep", "", 8};
    spec.sampleStrideFrames = 4;
    RealtimeAnalysisCapture capture({std::move(spec)}, 8, 1);
    require(capture.ready() && capture.bindStage(81),
        "slow sweep should prepare a bounded stage-bound probe");
    const auto publish = [&](std::uint64_t frame,
                             std::span<const double> values) {
        return capture.publishRecord(0, frame,
            RealtimeAnalysisPayloadKind::AudioBlock, 1, values);
    };
    const std::array first{0.0, 1.0, 2.0};
    const std::array second{3.0, 4.0, 5.0};
    const std::array third{6.0, 7.0, 8.0};
    const std::array fourth{9.0, 10.0, 11.0};
    const std::array fifth{12.0, 13.0, 14.0};
    std::array<double, 8> output{};
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_release);
    const auto firstPublished = publish(0, first);
    const auto secondPublished = publish(3, second);
    const auto thirdPublished = publish(6, third);
    const auto fourthPublished = publish(9, fourth);
    const auto stale = capture.copyLatestRecord(0, output);
    const auto fifthPublished = publish(12, fifth);
    const auto fresh = capture.copyLatestRecord(0, output);
    countAllocations.store(false, std::memory_order_release);
    require(firstPublished && !secondPublished && !thirdPublished
            && fourthPublished && fifthPublished
            && stale.has_value() && stale->valueCount == 1
            && fresh.has_value() && fresh->valueCount == 4
            && fresh->absoluteSamplePosition == 0
            && output[0] == 0.0 && output[1] == 4.0
            && output[2] == 8.0 && output[3] == 12.0,
        "slow sweep should retain selected samples despite short callbacks and a full UI queue");
    require(capture.droppedBlockCount(0) == 2
            && allocationCount.load(std::memory_order_relaxed) == 0,
        "slow sweep must account for a dropped snapshot without allocating");

    const std::array jumped{100.0, 101.0, 102.0};
    require(publish(100, jumped), "transport jump should publish a new slow sample");
    const auto discontinuous = capture.copyLatestRecord(0, output);
    require(discontinuous.has_value()
            && discontinuous->absoluteSamplePosition == 25
            && discontinuous->valueCount == 1 && output[0] == 100.0,
        "transport jump must clear prior slow sweep history");
    require(capture.bindStage(82), "slow sweep should rebind for a new stage");
    const std::array restaged{200.0, 201.0};
    require(publish(200, restaged), "new stage should publish its first slow sample");
    const auto newStage = capture.copyLatestRecord(0, output);
    require(newStage.has_value() && newStage->stageId == 82
            && newStage->absoluteSamplePosition == 50
            && newStage->valueCount == 1 && output[0] == 200.0,
        "new stage must not expose prior slow sweep samples");
}

void testCaptureRejectsInvalidRealtimeCalls()
{
    RealtimeAnalysisCapture capture(
        {{"scope", 0, RealtimeAnalysisKind::Oscilloscope}},
        2,
        2);
    const std::array oversized{1.0, 2.0, 3.0};
    const std::array<double, 0> empty{};
    std::array<double, 2> output{};
    require(!capture.publish(1, output), "invalid producer probe should reject");
    require(!capture.publish(0, empty), "empty producer block should reject");
    require(!capture.publish(0, oversized), "oversized producer block should reject");
    require(!capture.copyLatest(1, output).has_value(), "invalid consumer probe should reject");
    require(capture.droppedBlockCount(1) == 0, "invalid drop query should return zero");
}

void testTypedCaptureBindsStageAndPublishesExactRecordMetadata()
{
    RealtimeAnalysisCapture capture(
        {{"partials", 7, RealtimeAnalysisKind::Spectrum,
          RealtimeAnalysisPayloadKind::PartialSpectrum, 3}},
        12,
        3);
    require(capture.ready(), "typed capture should prepare");
    require(capture.bindStage(41), "typed capture should bind a nonzero stage");
    require(capture.boundStageId() == 41
            && capture.bindingGeneration() == 1,
        "stage binding should publish exact stage and first generation");

    const std::array values{110.0, 0.5, 0.1, 220.0, 0.25, -0.2};
    require(capture.publishRecord(
                0,
                8192,
                RealtimeAnalysisPayloadKind::PartialSpectrum,
                3,
                values),
        "matching typed payload should publish");
    std::array<double, 12> output{};
    const auto record = capture.copyLatestRecord(0, output);
    require(record.has_value()
            && record->stageId == 41
            && record->bindingGeneration == 1
            && record->sequence == 1
            && record->absoluteSamplePosition == 8192
            && record->payloadKind
                == RealtimeAnalysisPayloadKind::PartialSpectrum
            && record->channelCount == 3
            && record->valueCount == values.size(),
        "consumer should receive exact stage-bound typed metadata");
    for (std::size_t index = 0; index < values.size(); ++index)
        requireNear(output[index], values[index], "typed payload value");
}

void testTypedCaptureRejectsShapeMismatchAndFlushesRebindEvidence()
{
    RealtimeAnalysisCapture capture(
        {{"events", 0, RealtimeAnalysisKind::Oscilloscope,
          RealtimeAnalysisPayloadKind::EventBatch, 3}},
        12,
        3);
    require(capture.bindStage(11), "first stage should bind");
    const std::array invalidShape{1.0, 2.0, 3.0, 4.0};
    require(!capture.publishRecord(
                0,
                0,
                RealtimeAnalysisPayloadKind::EventBatch,
                3,
                invalidShape)
            && capture.rejectedRecordCount(0) == 1,
        "typed capture should reject payloads that do not fit their shape");
    const std::array invalidKind{1.5, 1.0, 0.0};
    require(!capture.publishRecord(
                0,
                0,
                RealtimeAnalysisPayloadKind::EventBatch,
                3,
                invalidKind)
            && capture.rejectedRecordCount(0) == 2,
        "typed capture should reject a fractional Event kind token");
    const std::array invalidFrame{1.0, 1.0, 0.5};
    require(!capture.publishRecord(
                0,
                0,
                RealtimeAnalysisPayloadKind::EventBatch,
                3,
                invalidFrame)
            && capture.rejectedRecordCount(0) == 3,
        "typed capture should reject a fractional Event frame offset");
    const std::array valid{1.0, 2.0, 3.0};
    require(capture.publishRecord(
                0,
                10,
                RealtimeAnalysisPayloadKind::EventBatch,
                3,
                valid),
        "valid old-stage record should publish");
    require(capture.bindStage(12)
            && capture.bindingGeneration() == 2,
        "rebinding should advance the evidence generation");
    std::array<double, 12> output{};
    require(!capture.copyLatestRecord(0, output).has_value(),
        "rebinding should flush every queued record from the prior stage");
    require(capture.publishRecord(
                0,
                20,
                RealtimeAnalysisPayloadKind::EventBatch,
                3,
                valid),
        "new-stage record should publish after flush");
    const auto next = capture.copyLatestRecord(0, output);
    require(next.has_value() && next->stageId == 12
            && next->bindingGeneration == 2,
        "post-rebind evidence should carry only the new identity");
    require(!capture.bindStage(0), "zero stage binding should reject");
}

void testTypedCapturePreservesEmptyPartialFramesAndRejectsOddComplexPayloads()
{
    RealtimeAnalysisCapture partials(
        {{"partials", 0, RealtimeAnalysisKind::Spectrum,
          RealtimeAnalysisPayloadKind::PartialSpectrum, 1}},
        12,
        2);
    require(partials.bindStage(15),
        "empty PartialSpectrum fixture should bind");
    const std::array<double, 0> empty{};
    require(partials.publishRecord(
                0,
                100,
                RealtimeAnalysisPayloadKind::PartialSpectrum,
                1,
                empty),
        "an exact empty PartialSpectrum frame should remain observable");
    std::array<double, 12> output{};
    const auto emptyRecord = partials.copyLatestRecord(0, output);
    require(emptyRecord.has_value()
            && emptyRecord->absoluteSamplePosition == 100
            && emptyRecord->valueCount == 0,
        "empty PartialSpectrum evidence should retain its frame identity");

    RealtimeAnalysisCapture complex(
        {{"complex", 0, RealtimeAnalysisKind::Spectrum,
          RealtimeAnalysisPayloadKind::ComplexFrame, 1}},
        12,
        2);
    require(complex.bindStage(16),
        "ComplexFrame shape fixture should bind");
    const std::array odd{1.0, 0.0, 2.0};
    require(!complex.publishRecord(
                0,
                0,
                RealtimeAnalysisPayloadKind::ComplexFrame,
                1,
                odd)
            && complex.rejectedRecordCount(0) == 1,
        "ComplexFrame should reject a non-paired real/imaginary payload");
}

void testTypedConsumerReportsExactNoRecordAndCallerFailures()
{
    RealtimeAnalysisCapture capture(
        {{"control", 0, RealtimeAnalysisKind::Oscilloscope,
          RealtimeAnalysisPayloadKind::ControlBlock, 1}},
        4,
        2);
    require(capture.bindStage(19),
        "typed consumer status fixture should bind");
    std::array<double, 4> destination{};
    require(capture.copyLatestRecordResult(0, destination).status
            == RealtimeAnalysisCopyStatus::NoRecord,
        "empty current-stage queue should report exact no-record");
    require(capture.copyLatestRecordResult(1, destination).status
            == RealtimeAnalysisCopyStatus::InvalidProbe,
        "invalid consumer probe should not look like no-new-data");
    capture.rejectPreparedRecord(0);
    require(capture.rejectedRecordCount(0) == 1,
        "prepared producer should count one explicitly rejected record");

    const std::array nonfinite{
        1.0,
        2.0,
        std::numeric_limits<double>::infinity(),
        4.0,
    };
    require(!capture.publishRecord(
                0,
                30,
                RealtimeAnalysisPayloadKind::ControlBlock,
                1,
                nonfinite)
            && capture.rejectedRecordCount(0) == 2,
        "typed capture should reject non-finite display evidence");

    const std::array values{1.0, 2.0, 3.0, 4.0};
    require(capture.publishRecord(
                0,
                40,
                RealtimeAnalysisPayloadKind::ControlBlock,
                1,
                values),
        "typed consumer status record should publish");
    std::array<double, 2> tooSmall{};
    require(capture.copyLatestRecordResult(0, tooSmall).status
            == RealtimeAnalysisCopyStatus::DestinationTooSmall,
        "undersized destination should report exact caller capacity failure");
    const auto accepted = capture.copyLatestRecordResult(0, destination);
    require(accepted.status == RealtimeAnalysisCopyStatus::Accepted
            && accepted.header.absoluteSamplePosition == 40
            && accepted.header.valueCount == values.size(),
        "adequate destination should still consume the retained exact record");
}

void testCapturePublicationAndConsumptionDoNotAllocate()
{
    RealtimeAnalysisCapture capture(
        {{"scope", 0, RealtimeAnalysisKind::Oscilloscope}},
        4,
        2);
    const std::array input{1.0, 2.0, 3.0, 4.0};
    std::array<double, 4> output{};

    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_release);
    const auto published = capture.publish(0, input);
    const auto copied = capture.copyLatest(0, output);
    countAllocations.store(false, std::memory_order_release);

    require(published && copied == std::optional<std::size_t>{4},
            "allocation test should publish and consume a block");
    require(allocationCount.load(std::memory_order_relaxed) == 0,
            "prepared producer and consumer calls should not allocate");

    RealtimeAnalysisCapture typed(
        {{"complex", 0, RealtimeAnalysisKind::Spectrum,
          RealtimeAnalysisPayloadKind::ComplexFrame, 2}},
        4,
        2);
    require(typed.bindStage(17), "typed allocation fixture should bind");
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_release);
    const auto typedPublished = typed.publishRecord(
        0,
        32,
        RealtimeAnalysisPayloadKind::ComplexFrame,
        2,
        input);
    const auto typedCopied = typed.copyLatestRecord(0, output);
    countAllocations.store(false, std::memory_order_release);
    require(typedPublished && typedCopied.has_value(),
        "typed allocation test should publish and consume");
    require(allocationCount.load(std::memory_order_relaxed) == 0,
        "typed prepared producer and consumer calls should not allocate");
}

void testCaptureSupportsConcurrentSingleProducerAndConsumer()
{
    RealtimeAnalysisCapture capture(
        {{"scope", 0, RealtimeAnalysisKind::Oscilloscope}},
        1,
        8);
    std::atomic<bool> producerDone = false;
    std::atomic<bool> consumerFailed = false;
    std::atomic<double> newestValue = -1.0;
    constexpr std::size_t blockCount = 10000;

    std::thread producer([&] {
        for (std::size_t index = 0; index < blockCount; ++index)
        {
            const std::array block{static_cast<double>(index)};
            while (!capture.publish(0, block))
                std::this_thread::yield();
        }
        producerDone.store(true, std::memory_order_release);
    });
    std::thread consumer([&] {
        std::array<double, 1> block{};
        double previous = -1.0;
        while (true)
        {
            const auto copied = capture.copyLatest(0, block);
            if (!copied.has_value())
            {
                if (producerDone.load(std::memory_order_acquire))
                    break;
                std::this_thread::yield();
                continue;
            }
            if (block[0] <= previous)
                consumerFailed.store(true, std::memory_order_relaxed);
            previous = block[0];
            newestValue.store(previous, std::memory_order_relaxed);
        }
    });
    producer.join();
    consumer.join();

    require(!consumerFailed.load(std::memory_order_relaxed),
            "concurrent newest-block values should be strictly increasing");
    requireNear(newestValue.load(std::memory_order_relaxed),
                static_cast<double>(blockCount - 1),
                "consumer should eventually observe final producer block");
}

void testTypedPartialCapturePreservesIntegerTrackIdentity()
{
    RealtimeAnalysisCapture capture(
        {{"partial", 0, RealtimeAnalysisKind::Spectrum,
          RealtimeAnalysisPayloadKind::PartialSpectrum,
          1, "spectrumOut", "typed-partials"}},
        768,
        2);
    require(capture.ready() && capture.bindStage(71),
            "typed partial capture should bind exact stage authority");
    const std::array<talas::dsp::PartialSpectrumPoint, 2> points{{
        {true, 9007199254740991ULL, 0.5, 440.0, 8.0},
        {false, 0, 0.25, 880.0, 12.0},
    }};
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_release);
    const auto published = capture.publishPartialSpectrum(
        0, 256, points);
    std::array<talas::dsp::PartialSpectrumPoint, 2> copied{};
    const auto result = capture.copyLatestPartialSpectrum(0, copied);
    countAllocations.store(false, std::memory_order_release);
    require(published
                && result.status == RealtimeAnalysisCopyStatus::Accepted
                && result.header.stageId == 71
                && result.header.absoluteSamplePosition == 256
                && result.pointCount == 2
                && copied[0].hasTrackId
                && copied[0].trackId == points[0].trackId
                && copied[0].amplitude == points[0].amplitude
                && copied[0].frequencyHz == points[0].frequencyHz
                && copied[0].bandwidthHz == points[0].bandwidthHz
                && !copied[1].hasTrackId
                && copied[1].trackId == 0
                && copied[1].frequencyHz == points[1].frequencyHz,
            "typed partial capture must preserve optional uint64 track IDs exactly");
    require(allocationCount.load(std::memory_order_relaxed) == 0,
            "typed partial publication and consumption should not allocate");
}
}

int main()
{
    testCaptureRejectsInvalidPreparation();
    testCaptureFindsDeterministicProbeIdentity();
    testCapturePublishesAndDrainsNewestCompleteBlock();
    testCaptureDropsWhenFullAndRetainsBlockForLargeEnoughDestination();
    testRollingOscilloscopeHistorySurvivesSkippedCallbacksAndResets();
    testDecimatedSweepRetainsSlowHistoryAcrossShortCallbacks();
    testCaptureRejectsInvalidRealtimeCalls();
    testTypedCaptureBindsStageAndPublishesExactRecordMetadata();
    testTypedCaptureRejectsShapeMismatchAndFlushesRebindEvidence();
    testTypedCapturePreservesEmptyPartialFramesAndRejectsOddComplexPayloads();
    testTypedConsumerReportsExactNoRecordAndCallerFailures();
    testCapturePublicationAndConsumptionDoNotAllocate();
    testCaptureSupportsConcurrentSingleProducerAndConsumer();
    testTypedPartialCapturePreservesIntegerTrackIdentity();
    return 0;
}
