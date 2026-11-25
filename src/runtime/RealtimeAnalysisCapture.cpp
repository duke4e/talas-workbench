#include "talas/runtime/RealtimeAnalysisCapture.h"
#include "talas/runtime/TypedEvent.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <ranges>
#include <utility>

namespace talas::runtime
{
namespace
{
void incrementSingleWriterSaturated(
    std::atomic<std::uint64_t>& counter) noexcept
{
    const auto current = counter.load(std::memory_order_relaxed);
    if (current != std::numeric_limits<std::uint64_t>::max())
        counter.store(current + 1, std::memory_order_relaxed);
}

[[nodiscard]] bool validPayloadValueCount(
    RealtimeAnalysisPayloadKind kind,
    std::size_t valueCount,
    std::uint32_t channelCount,
    std::size_t capacity) noexcept
{
    if (channelCount == 0 || valueCount > capacity)
        return false;
    switch (kind)
    {
    case RealtimeAnalysisPayloadKind::AudioBlock:
    case RealtimeAnalysisPayloadKind::ControlBlock:
        return valueCount != 0
            && valueCount % channelCount == 0;
    case RealtimeAnalysisPayloadKind::EventBatch:
        return valueCount <= maximumRealtimeAnalysisEventCount * 3
            && valueCount % 3 == 0;
    case RealtimeAnalysisPayloadKind::PartialSpectrum:
        return valueCount <= maximumRealtimeAnalysisPartialCount * 3
            && valueCount % 3 == 0;
    case RealtimeAnalysisPayloadKind::ComplexFrame:
        return channelCount <= maximumRealtimeAnalysisComplexChannelCount
            && valueCount != 0
            && valueCount
                <= maximumRealtimeAnalysisComplexBinsPerChannel
                    * channelCount * 2
            && valueCount % (channelCount * 2) == 0;
    }
    return false;
}

[[nodiscard]] bool validPayloadValues(
    RealtimeAnalysisPayloadKind kind,
    std::span<const double> values) noexcept
{
    if (!std::ranges::all_of(
            values,
            [](double value) { return std::isfinite(value); }))
    {
        return false;
    }
    if (kind != RealtimeAnalysisPayloadKind::EventBatch)
        return true;
    for (std::size_t index = 0; index < values.size(); index += 3)
    {
        const auto eventKind = values[index];
        const auto frameOffset = values[index + 2];
        if (eventKind != std::floor(eventKind)
            || eventKind < 0.0
            || eventKind
                > static_cast<double>(
                    TypedEventKind::TransportDiscontinuity)
            || frameOffset != std::floor(frameOffset)
            || frameOffset < 0.0)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool validPartialPoint(
    const talas::dsp::PartialSpectrumPoint& point) noexcept
{
    return (!point.hasTrackId || point.trackId != 0)
        && (point.hasTrackId || point.trackId == 0)
        && std::isfinite(point.amplitude)
        && point.amplitude >= 0.0
        && std::isfinite(point.frequencyHz)
        && point.frequencyHz > 0.0
        && std::isfinite(point.bandwidthHz)
        && point.bandwidthHz >= 0.0;
}
}

struct RealtimeAnalysisCapture::Probe
{
    Probe(
        RealtimeAnalysisProbeSpec probeSpec,
        std::size_t maxBlockFrames,
        std::size_t queueCapacity)
        : spec(std::move(probeSpec))
        , samples(maxBlockFrames * queueCapacity, 0.0)
        , headers(queueCapacity)
        , history(spec.historyFrames, 0.0)
        , decimatedScratch(
              spec.sampleStrideFrames > 1 ? maxBlockFrames : 0, 0.0)
        , partials(maximumRealtimeAnalysisPartialCount * queueCapacity)
        , partialHeaders(queueCapacity)
    {
    }

    RealtimeAnalysisProbeSpec spec;
    std::vector<double> samples;
    std::vector<RealtimeAnalysisRecordHeader> headers;
    std::vector<double> history;
    std::vector<double> decimatedScratch;
    std::size_t historySize = 0;
    std::uint64_t historyEnd = 0;
    std::uint64_t sourceEnd = 0;
    std::uint64_t historyStage = 0;
    std::uint64_t historyGeneration = 0;
    std::vector<talas::dsp::PartialSpectrumPoint> partials;
    std::vector<RealtimeAnalysisRecordHeader> partialHeaders;
    std::atomic<std::uint64_t> writeIndex = 0;
    std::atomic<std::uint64_t> readIndex = 0;
    std::atomic<std::uint64_t> partialWriteIndex = 0;
    std::atomic<std::uint64_t> partialReadIndex = 0;
    std::atomic<std::uint64_t> droppedBlocks = 0;
    std::atomic<std::uint64_t> producerRejectedRecords = 0;
    std::atomic<std::uint64_t> consumerRejectedRecords = 0;
};

RealtimeAnalysisCapture::RealtimeAnalysisCapture(
    std::vector<RealtimeAnalysisProbeSpec> probes,
    std::size_t maxBlockFrames,
    std::size_t queueCapacity)
    : maxBlockFrames_(maxBlockFrames)
    , queueCapacity_(queueCapacity)
{
    if (probes.size() > maximumRealtimeAnalysisProbeCount)
        return;
    if (probes.empty())
    {
        ready_ = true;
        return;
    }
    if (maxBlockFrames == 0
        || maxBlockFrames > maximumRealtimeAnalysisPayloadValues
        || queueCapacity == 0
        || queueCapacity > maximumRealtimeAnalysisQueueCapacity
        || maxBlockFrames > std::numeric_limits<std::size_t>::max() / queueCapacity)
    {
        return;
    }
    for (std::size_t index = 0; index < probes.size(); ++index)
    {
        if (probes[index].nodeId.empty()
            || probes[index].portId.empty()
            || probes[index].channelCount == 0
            || probes[index].channelCount > 8
            || probes[index].sampleStrideFrames == 0
            || (probes[index].sampleStrideFrames > 1
                && probes[index].historyFrames == 0)
            || (probes[index].historyFrames != 0
                && (probes[index].historyFrames > realtimeOscilloscopeHistoryFrames
                    || probes[index].historyFrames > maxBlockFrames
                    || probes[index].kind != RealtimeAnalysisKind::Oscilloscope
                    || probes[index].payloadKind
                        != RealtimeAnalysisPayloadKind::AudioBlock
                    || probes[index].channelCount != 1)))
            return;
        const auto duplicate = std::ranges::find_if(
            probes.begin(),
            probes.begin() + static_cast<std::ptrdiff_t>(index),
            [&](const auto& candidate) {
                return candidate.nodeId == probes[index].nodeId
                    && candidate.portId == probes[index].portId
                    && candidate.scopeId == probes[index].scopeId
                    && candidate.payloadKind == probes[index].payloadKind;
            });
        if (duplicate != probes.begin() + static_cast<std::ptrdiff_t>(index))
            return;
        if (!probes[index].clientId.empty())
        {
            const auto duplicateClient = std::ranges::find(
                probes.begin(),
                probes.begin() + static_cast<std::ptrdiff_t>(index),
                probes[index].clientId,
                &RealtimeAnalysisProbeSpec::clientId);
            if (duplicateClient
                != probes.begin() + static_cast<std::ptrdiff_t>(index))
            {
                return;
            }
        }
    }

    probes_.reserve(probes.size());
    for (auto& probe : probes)
        probes_.push_back(std::make_unique<Probe>(std::move(probe), maxBlockFrames, queueCapacity));
    ready_ = true;
}

RealtimeAnalysisCapture::~RealtimeAnalysisCapture() = default;

bool RealtimeAnalysisCapture::ready() const noexcept
{
    return ready_;
}

std::size_t RealtimeAnalysisCapture::maxBlockFrames() const noexcept
{
    return maxBlockFrames_;
}

std::size_t RealtimeAnalysisCapture::maxPayloadValues() const noexcept
{
    return maxBlockFrames_;
}

std::size_t RealtimeAnalysisCapture::queueCapacity() const noexcept
{
    return queueCapacity_;
}

std::size_t RealtimeAnalysisCapture::probeCount() const noexcept
{
    return probes_.size();
}

const RealtimeAnalysisProbeSpec* RealtimeAnalysisCapture::probeSpec(std::size_t probeIndex) const noexcept
{
    return probeIndex < probes_.size() ? &probes_[probeIndex]->spec : nullptr;
}

std::optional<std::size_t> RealtimeAnalysisCapture::probeIndex(
    std::string_view nodeId,
    std::uint32_t scopeId) const noexcept
{
    const auto found = std::ranges::find_if(probes_, [&](const auto& probe) {
        return probe->spec.nodeId == nodeId && probe->spec.scopeId == scopeId;
    });
    if (found == probes_.end())
        return std::nullopt;
    return static_cast<std::size_t>(std::distance(probes_.begin(), found));
}

std::optional<std::size_t>
RealtimeAnalysisCapture::probeIndexForClientId(
    std::string_view clientId) const noexcept
{
    if (clientId.empty())
        return std::nullopt;
    const auto found = std::ranges::find_if(probes_, [&](const auto& probe) {
        return probe->spec.clientId == clientId;
    });
    if (found == probes_.end())
        return std::nullopt;
    return static_cast<std::size_t>(
        std::distance(probes_.begin(), found));
}

std::optional<std::size_t> RealtimeAnalysisCapture::probeIndex(
    std::string_view nodeId,
    std::string_view portId,
    std::uint32_t scopeId,
    RealtimeAnalysisPayloadKind payloadKind) const noexcept
{
    const auto found = std::ranges::find_if(probes_, [&](const auto& probe) {
        return probe->spec.nodeId == nodeId
            && probe->spec.portId == portId
            && probe->spec.scopeId == scopeId
            && probe->spec.payloadKind == payloadKind;
    });
    if (found == probes_.end())
        return std::nullopt;
    return static_cast<std::size_t>(std::distance(probes_.begin(), found));
}

std::optional<std::size_t> RealtimeAnalysisCapture::preferredTriggerProbeIndex(
    std::size_t displayProbeIndex) const noexcept
{
    const auto* display = probeSpec(displayProbeIndex);
    if (display == nullptr
        || display->kind != RealtimeAnalysisKind::Oscilloscope
        || display->payloadKind != RealtimeAnalysisPayloadKind::AudioBlock
        || display->portId != "audioOut")
        return std::nullopt;
    const auto direct = probeIndex(
        display->nodeId, "sweepOrTrigOut", display->scopeId,
        RealtimeAnalysisPayloadKind::AudioBlock);
    if (direct.has_value())
        return direct;
    return display->triggerOnRight
        ? probeIndex(
            display->nodeId, "rightOut", display->scopeId,
            RealtimeAnalysisPayloadKind::AudioBlock)
        : std::nullopt;
}

std::optional<std::size_t> RealtimeAnalysisCapture::preferredProbeIndex(
    std::string_view nodeId) const noexcept
{
    std::optional<std::size_t> preferred;
    std::uint32_t preferredScope = std::numeric_limits<std::uint32_t>::max();
    for (std::size_t index = 0; index < probes_.size(); ++index)
    {
        const auto& spec = probes_[index]->spec;
        if (spec.nodeId != nodeId || (preferred.has_value() && spec.scopeId >= preferredScope))
            continue;
        preferred = index;
        preferredScope = spec.scopeId;
    }
    return preferred;
}

bool RealtimeAnalysisCapture::bindStage(std::uint64_t stageId) noexcept
{
    if (!ready_ || stageId == 0)
        return false;
    const auto generation = bindingGeneration_.load(std::memory_order_relaxed);
    if (generation == std::numeric_limits<std::uint64_t>::max())
        return false;
    for (auto& probe : probes_)
    {
        const auto write = probe->writeIndex.load(std::memory_order_acquire);
        probe->readIndex.store(write, std::memory_order_release);
        const auto partialWrite = probe->partialWriteIndex.load(
            std::memory_order_acquire);
        probe->partialReadIndex.store(
            partialWrite, std::memory_order_release);
        probe->historySize = 0;
        probe->historyEnd = 0;
        probe->sourceEnd = 0;
        probe->historyStage = 0;
        probe->historyGeneration = 0;
    }
    boundStageId_.store(stageId, std::memory_order_relaxed);
    bindingGeneration_.store(generation + 1, std::memory_order_release);
    return true;
}

std::uint64_t RealtimeAnalysisCapture::boundStageId() const noexcept
{
    return boundStageId_.load(std::memory_order_acquire);
}

std::uint64_t RealtimeAnalysisCapture::bindingGeneration() const noexcept
{
    return bindingGeneration_.load(std::memory_order_acquire);
}

bool RealtimeAnalysisCapture::publish(
    std::size_t probeIndex,
    std::span<const double> samples) noexcept
{
    if (!ready_ || probeIndex >= probes_.size())
        return false;
    const auto& spec = probes_[probeIndex]->spec;
    if (spec.payloadKind != RealtimeAnalysisPayloadKind::AudioBlock)
    {
        incrementSingleWriterSaturated(
            probes_[probeIndex]->producerRejectedRecords);
        return false;
    }
    return publishRecord(
        probeIndex,
        0,
        RealtimeAnalysisPayloadKind::AudioBlock,
        spec.channelCount,
        samples);
}

bool RealtimeAnalysisCapture::publishRecord(
    std::size_t probeIndex,
    std::uint64_t absoluteSamplePosition,
    RealtimeAnalysisPayloadKind payloadKind,
    std::uint32_t channelCount,
    std::span<const double> values) noexcept
{
    if (!ready_ || probeIndex >= probes_.size())
        return false;

    auto& probe = *probes_[probeIndex];
    if (!validPayloadValueCount(
            payloadKind,
            values.size(),
            channelCount,
            maxBlockFrames_)
        || !validPayloadValues(payloadKind, values)
        || payloadKind != probe.spec.payloadKind
        || channelCount != probe.spec.channelCount)
    {
        incrementSingleWriterSaturated(probe.producerRejectedRecords);
        return false;
    }
    const auto bindingGeneration =
        bindingGeneration_.load(std::memory_order_acquire);
    const auto stageId = boundStageId_.load(std::memory_order_relaxed);
    std::span<const double> publishedValues = values;
    auto publishedPosition = absoluteSamplePosition;
    if (!probe.history.empty())
    {
        if (values.size()
            > std::numeric_limits<std::uint64_t>::max()
                - absoluteSamplePosition)
        {
            incrementSingleWriterSaturated(probe.producerRejectedRecords);
            return false;
        }
        const auto resetHistory = probe.historyStage != stageId
            || probe.historyGeneration != bindingGeneration
            || (probe.historySize != 0
                && absoluteSamplePosition != probe.sourceEnd);
        if (resetHistory)
            probe.historySize = 0;
        probe.sourceEnd = absoluteSamplePosition + values.size();
        probe.historyStage = stageId;
        probe.historyGeneration = bindingGeneration;
        auto firstPosition = absoluteSamplePosition;
        if (probe.spec.sampleStrideFrames > 1)
        {
            const auto stride = static_cast<std::uint64_t>(
                probe.spec.sampleStrideFrames);
            const auto remainder = absoluteSamplePosition % stride;
            const auto firstOffset = remainder == 0 ? 0 : stride - remainder;
            std::size_t selectedCount = 0;
            for (auto offset = firstOffset; offset < values.size(); offset += stride)
                probe.decimatedScratch[selectedCount++] = values[
                    static_cast<std::size_t>(offset)];
            if (selectedCount == 0)
                return true;
            firstPosition = (absoluteSamplePosition + firstOffset) / stride;
            values = std::span<const double>{
                probe.decimatedScratch.data(), selectedCount};
        }
        if (probe.historySize == 0)
            probe.historyEnd = firstPosition;
        const auto capacity = probe.history.size();
        if (values.size() >= capacity)
        {
            std::copy_n(values.end() - static_cast<std::ptrdiff_t>(capacity),
                capacity, probe.history.begin());
            probe.historySize = capacity;
        }
        else
        {
            const auto retained = std::min(
                probe.historySize, capacity - values.size());
            if (retained != 0)
            {
                std::copy_n(
                    probe.history.begin()
                        + static_cast<std::ptrdiff_t>(probe.historySize - retained),
                    retained, probe.history.begin());
            }
            std::copy(values.begin(), values.end(),
                probe.history.begin() + static_cast<std::ptrdiff_t>(retained));
            probe.historySize = retained + values.size();
        }
        probe.historyEnd = firstPosition + values.size();
        publishedPosition = probe.historyEnd - probe.historySize;
        publishedValues = std::span<const double>{
            probe.history.data(), probe.historySize};
    }
    const auto writeIndex = probe.writeIndex.load(std::memory_order_relaxed);
    const auto readIndex = probe.readIndex.load(std::memory_order_acquire);
    if (writeIndex == std::numeric_limits<std::uint64_t>::max()
        || writeIndex - readIndex >= queueCapacity_)
    {
        incrementSingleWriterSaturated(probe.droppedBlocks);
        return false;
    }

    const auto slot = static_cast<std::size_t>(writeIndex % queueCapacity_);
    std::copy(publishedValues.begin(), publishedValues.end(),
              probe.samples.begin() + static_cast<std::ptrdiff_t>(slot * maxBlockFrames_));
    probe.headers[slot] = {
        stageId,
        bindingGeneration,
        writeIndex + 1,
        publishedPosition,
        payloadKind,
        channelCount,
        static_cast<std::uint32_t>(publishedValues.size()),
    };
    probe.writeIndex.store(writeIndex + 1, std::memory_order_release);
    return true;
}

bool RealtimeAnalysisCapture::publishPartialSpectrum(
    std::size_t probeIndex,
    std::uint64_t absoluteSamplePosition,
    std::span<const talas::dsp::PartialSpectrumPoint> points) noexcept
{
    if (!ready_ || probeIndex >= probes_.size())
        return false;
    auto& probe = *probes_[probeIndex];
    if (probe.spec.payloadKind
            != RealtimeAnalysisPayloadKind::PartialSpectrum
        || probe.spec.channelCount != 1
        || points.size() > maximumRealtimeAnalysisPartialCount
        || !std::ranges::all_of(points, validPartialPoint))
    {
        incrementSingleWriterSaturated(probe.producerRejectedRecords);
        return false;
    }
    const auto bindingGeneration =
        bindingGeneration_.load(std::memory_order_acquire);
    const auto stageId = boundStageId_.load(std::memory_order_relaxed);
    const auto writeIndex = probe.partialWriteIndex.load(
        std::memory_order_relaxed);
    const auto readIndex = probe.partialReadIndex.load(
        std::memory_order_acquire);
    if (writeIndex == std::numeric_limits<std::uint64_t>::max()
        || writeIndex - readIndex >= queueCapacity_)
    {
        incrementSingleWriterSaturated(probe.droppedBlocks);
        return false;
    }
    const auto slot = static_cast<std::size_t>(
        writeIndex % queueCapacity_);
    std::copy(
        points.begin(),
        points.end(),
        probe.partials.begin()
            + static_cast<std::ptrdiff_t>(
                slot * maximumRealtimeAnalysisPartialCount));
    probe.partialHeaders[slot] = {
        stageId,
        bindingGeneration,
        writeIndex + 1,
        absoluteSamplePosition,
        RealtimeAnalysisPayloadKind::PartialSpectrum,
        1,
        static_cast<std::uint32_t>(points.size()),
    };
    probe.partialWriteIndex.store(
        writeIndex + 1, std::memory_order_release);
    return true;
}

void RealtimeAnalysisCapture::rejectPreparedRecord(
    std::size_t probeIndex) noexcept
{
    if (!ready_ || probeIndex >= probes_.size())
        return;
    incrementSingleWriterSaturated(
        probes_[probeIndex]->producerRejectedRecords);
}

std::optional<std::size_t> RealtimeAnalysisCapture::copyLatest(
    std::size_t probeIndex,
    std::span<double> destination) noexcept
{
    if (!ready_ || probeIndex >= probes_.size()
        || probes_[probeIndex]->spec.payloadKind
            != RealtimeAnalysisPayloadKind::AudioBlock)
        return std::nullopt;
    const auto record = copyLatestRecord(probeIndex, destination);
    return record.has_value()
        ? std::optional<std::size_t>{record->valueCount}
        : std::nullopt;
}

std::optional<RealtimeAnalysisRecordHeader>
RealtimeAnalysisCapture::copyLatestRecord(
    std::size_t probeIndex,
    std::span<double> destination) noexcept
{
    const auto result = copyLatestRecordResult(
        probeIndex,
        destination);
    return result.status == RealtimeAnalysisCopyStatus::Accepted
        ? std::optional<RealtimeAnalysisRecordHeader>{result.header}
        : std::nullopt;
}

RealtimeAnalysisCopyResult
RealtimeAnalysisCapture::copyLatestRecordResult(
    std::size_t probeIndex,
    std::span<double> destination) noexcept
{
    if (!ready_ || probeIndex >= probes_.size())
        return {RealtimeAnalysisCopyStatus::InvalidProbe, {}};

    auto& probe = *probes_[probeIndex];
    const auto readIndex = probe.readIndex.load(std::memory_order_relaxed);
    const auto writeIndex = probe.writeIndex.load(std::memory_order_acquire);
    if (readIndex == writeIndex)
        return {RealtimeAnalysisCopyStatus::NoRecord, {}};

    const auto newestIndex = writeIndex - 1;
    const auto slot = static_cast<std::size_t>(newestIndex % queueCapacity_);
    const auto header = probe.headers[slot];
    if (!validPayloadValueCount(
            header.payloadKind,
            header.valueCount,
            header.channelCount,
            maxBlockFrames_)
        || header.payloadKind != probe.spec.payloadKind
        || header.channelCount != probe.spec.channelCount
        )
    {
        probe.readIndex.store(writeIndex, std::memory_order_release);
        incrementSingleWriterSaturated(probe.consumerRejectedRecords);
        return {RealtimeAnalysisCopyStatus::RejectedEvidence, {}};
    }
    if (destination.size() < header.valueCount)
    {
        return {
            RealtimeAnalysisCopyStatus::DestinationTooSmall,
            header,
        };
    }
    const auto currentGeneration =
        bindingGeneration_.load(std::memory_order_acquire);
    const auto currentStage = boundStageId_.load(std::memory_order_relaxed);
    if (header.bindingGeneration != currentGeneration
        || header.stageId != currentStage)
    {
        probe.readIndex.store(writeIndex, std::memory_order_release);
        incrementSingleWriterSaturated(probe.consumerRejectedRecords);
        return {RealtimeAnalysisCopyStatus::RejectedEvidence, {}};
    }

    std::copy_n(
        probe.samples.begin() + static_cast<std::ptrdiff_t>(slot * maxBlockFrames_),
        header.valueCount,
        destination.begin());
    probe.readIndex.store(writeIndex, std::memory_order_release);
    return {RealtimeAnalysisCopyStatus::Accepted, header};
}

RealtimePartialSpectrumCopyResult
RealtimeAnalysisCapture::copyLatestPartialSpectrum(
    std::size_t probeIndex,
    std::span<talas::dsp::PartialSpectrumPoint> destination) noexcept
{
    if (!ready_ || probeIndex >= probes_.size())
        return {RealtimeAnalysisCopyStatus::InvalidProbe, {}, 0};
    auto& probe = *probes_[probeIndex];
    if (probe.spec.payloadKind
        != RealtimeAnalysisPayloadKind::PartialSpectrum)
    {
        return {RealtimeAnalysisCopyStatus::InvalidProbe, {}, 0};
    }
    const auto readIndex = probe.partialReadIndex.load(
        std::memory_order_relaxed);
    const auto writeIndex = probe.partialWriteIndex.load(
        std::memory_order_acquire);
    if (readIndex == writeIndex)
        return {RealtimeAnalysisCopyStatus::NoRecord, {}, 0};
    const auto newestIndex = writeIndex - 1;
    const auto slot = static_cast<std::size_t>(
        newestIndex % queueCapacity_);
    const auto header = probe.partialHeaders[slot];
    if (header.payloadKind
            != RealtimeAnalysisPayloadKind::PartialSpectrum
        || header.channelCount != 1
        || header.valueCount > maximumRealtimeAnalysisPartialCount)
    {
        probe.partialReadIndex.store(
            writeIndex, std::memory_order_release);
        incrementSingleWriterSaturated(probe.consumerRejectedRecords);
        return {RealtimeAnalysisCopyStatus::RejectedEvidence, {}, 0};
    }
    if (destination.size() < header.valueCount)
    {
        return {
            RealtimeAnalysisCopyStatus::DestinationTooSmall,
            header,
            header.valueCount};
    }
    const auto currentGeneration =
        bindingGeneration_.load(std::memory_order_acquire);
    const auto currentStage = boundStageId_.load(
        std::memory_order_relaxed);
    if (header.bindingGeneration != currentGeneration
        || header.stageId != currentStage)
    {
        probe.partialReadIndex.store(
            writeIndex, std::memory_order_release);
        incrementSingleWriterSaturated(probe.consumerRejectedRecords);
        return {RealtimeAnalysisCopyStatus::RejectedEvidence, {}, 0};
    }
    std::copy_n(
        probe.partials.begin()
            + static_cast<std::ptrdiff_t>(
                slot * maximumRealtimeAnalysisPartialCount),
        header.valueCount,
        destination.begin());
    if (!std::ranges::all_of(
            destination.first(header.valueCount),
            validPartialPoint))
    {
        probe.partialReadIndex.store(
            writeIndex, std::memory_order_release);
        incrementSingleWriterSaturated(probe.consumerRejectedRecords);
        return {RealtimeAnalysisCopyStatus::RejectedEvidence, {}, 0};
    }
    probe.partialReadIndex.store(
        writeIndex, std::memory_order_release);
    return {
        RealtimeAnalysisCopyStatus::Accepted,
        header,
        header.valueCount};
}

std::uint64_t RealtimeAnalysisCapture::droppedBlockCount(std::size_t probeIndex) const noexcept
{
    return probeIndex < probes_.size()
        ? probes_[probeIndex]->droppedBlocks.load(std::memory_order_relaxed)
        : 0;
}

std::uint64_t RealtimeAnalysisCapture::rejectedRecordCount(
    std::size_t probeIndex) const noexcept
{
    if (probeIndex >= probes_.size())
        return 0;
    const auto producer = probes_[probeIndex]
        ->producerRejectedRecords.load(std::memory_order_relaxed);
    const auto consumer = probes_[probeIndex]
        ->consumerRejectedRecords.load(std::memory_order_relaxed);
    return consumer > std::numeric_limits<std::uint64_t>::max() - producer
        ? std::numeric_limits<std::uint64_t>::max()
        : producer + consumer;
}
}
