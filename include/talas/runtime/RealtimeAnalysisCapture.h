#pragma once

#include "talas/dsp/SpectralFrame.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace talas::runtime
{
enum class RealtimeAnalysisKind : std::uint8_t
{
    Oscilloscope,
    Spectrum,
};

enum class RealtimeAnalysisPayloadKind : std::uint8_t
{
    AudioBlock,
    ControlBlock,
    EventBatch,
    PartialSpectrum,
    ComplexFrame,
};

inline constexpr std::size_t maximumRealtimeAnalysisProbeCount = 16;
inline constexpr std::size_t maximumRealtimeAnalysisQueueCapacity = 8;
inline constexpr std::size_t maximumRealtimeAnalysisPayloadValues = 4096;
inline constexpr std::size_t maximumRealtimeAnalysisEventCount = 256;
inline constexpr std::size_t maximumRealtimeAnalysisPartialCount = 512;
inline constexpr std::size_t maximumRealtimeAnalysisComplexChannelCount = 2;
inline constexpr std::size_t maximumRealtimeAnalysisComplexBinsPerChannel =
    2048;
inline constexpr std::size_t realtimeOscilloscopeHistoryFrames = 512;
inline constexpr std::string_view realtimeAnalysisTypedEventEndpoint =
    "@typed-events";

struct RealtimeAnalysisRecordHeader
{
    std::uint64_t stageId = 0;
    std::uint64_t bindingGeneration = 0;
    std::uint64_t sequence = 0;
    std::uint64_t absoluteSamplePosition = 0;
    RealtimeAnalysisPayloadKind payloadKind =
        RealtimeAnalysisPayloadKind::AudioBlock;
    std::uint32_t channelCount = 1;
    std::uint32_t valueCount = 0;

    bool operator==(const RealtimeAnalysisRecordHeader&) const = default;
};

enum class RealtimeAnalysisCopyStatus : std::uint8_t
{
    NoRecord,
    Accepted,
    RejectedEvidence,
    DestinationTooSmall,
    InvalidProbe,
};

struct RealtimeAnalysisCopyResult
{
    RealtimeAnalysisCopyStatus status =
        RealtimeAnalysisCopyStatus::NoRecord;
    RealtimeAnalysisRecordHeader header;
};

struct RealtimePartialSpectrumCopyResult
{
    RealtimeAnalysisCopyStatus status =
        RealtimeAnalysisCopyStatus::NoRecord;
    RealtimeAnalysisRecordHeader header;
    std::size_t pointCount = 0;
};

struct RealtimeAnalysisProbeSpec
{
    std::string nodeId;
    std::uint32_t scopeId = 0;
    RealtimeAnalysisKind kind = RealtimeAnalysisKind::Oscilloscope;
    RealtimeAnalysisPayloadKind payloadKind =
        RealtimeAnalysisPayloadKind::AudioBlock;
    std::uint32_t channelCount = 1;
    std::string portId = "audioOut";
    std::string clientId;
    // Zero retains raw callback records. Oscilloscope probes may publish a
    // bounded rolling mono window without requiring the UI to drain every block.
    std::size_t historyFrames = 0;
    // Static Sound field used by the Workbench scope's Sweep mode.
    double sweepFrequencyHz = 30.0;
    // A display can show its input before applying gain to its audio output.
    std::string sourceInputPortId;
    // When the Sound has two audio lanes, use its right lane for edge detection.
    bool triggerOnRight = false;
    bool triggerModeEnabled = false;
    bool triggerOnFallingEdge = false;
    // Authored initial timebase. A sub-audio value prepares a second,
    // downsampled sweep probe while the trigger probe stays at audio rate.
    double initialSweepFrequencyHz = 30.0;
    // One published sample per this many input frames. Positions in a
    // decimated probe's records are sample indices at the reduced rate.
    std::uint32_t sampleStrideFrames = 1;
};

struct RealtimeAnalysisTargetSpec
{
    std::vector<std::string> instancePath;
    std::optional<std::string> containingDefinitionId;
    std::string nodeId;
    std::string portId = "audioOut";
    RealtimeAnalysisKind kind = RealtimeAnalysisKind::Oscilloscope;
    RealtimeAnalysisPayloadKind payloadKind =
        RealtimeAnalysisPayloadKind::AudioBlock;
    std::uint32_t channelCount = 1;
    std::string clientId;
    // With a clip selector, instancePath is relative to that clip's Sound.
    // An omitted iteration selects every repeat, each in its own capture.
    std::optional<std::string> scoreClipId;
    std::optional<std::uint32_t> scoreIterationOrdinal;
};

class RealtimeAnalysisCapture
{
public:
    RealtimeAnalysisCapture(
        std::vector<RealtimeAnalysisProbeSpec> probes,
        std::size_t maxBlockFrames,
        std::size_t queueCapacity);
    ~RealtimeAnalysisCapture();

    RealtimeAnalysisCapture(const RealtimeAnalysisCapture&) = delete;
    RealtimeAnalysisCapture& operator=(const RealtimeAnalysisCapture&) = delete;

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] std::size_t maxPayloadValues() const noexcept;
    // Compatibility name retained for the original AudioBlock-only API.
    [[nodiscard]] std::size_t maxBlockFrames() const noexcept;
    [[nodiscard]] std::size_t queueCapacity() const noexcept;
    [[nodiscard]] std::size_t probeCount() const noexcept;
    [[nodiscard]] const RealtimeAnalysisProbeSpec* probeSpec(std::size_t probeIndex) const noexcept;
    [[nodiscard]] std::optional<std::size_t> probeIndex(
        std::string_view nodeId,
        std::uint32_t scopeId) const noexcept;
    [[nodiscard]] std::optional<std::size_t> probeIndex(
        std::string_view nodeId,
        std::string_view portId,
        std::uint32_t scopeId,
        RealtimeAnalysisPayloadKind payloadKind) const noexcept;
    [[nodiscard]] std::optional<std::size_t> probeIndexForClientId(
        std::string_view clientId) const noexcept;
    [[nodiscard]] std::optional<std::size_t> preferredProbeIndex(
        std::string_view nodeId) const noexcept;
    [[nodiscard]] std::optional<std::size_t> preferredTriggerProbeIndex(
        std::size_t displayProbeIndex) const noexcept;

    // Control-thread only, before the corresponding engine is published to
    // the graph host. Rebinding advances a generation so queued evidence from
    // an earlier stage can never be projected as current.
    [[nodiscard]] bool bindStage(std::uint64_t stageId) noexcept;
    [[nodiscard]] std::uint64_t boundStageId() const noexcept;
    [[nodiscard]] std::uint64_t bindingGeneration() const noexcept;

    // Single audio-thread producer per capture object.
    [[nodiscard]] bool publish(
        std::size_t probeIndex,
        std::span<const double> samples) noexcept;
    [[nodiscard]] bool publishRecord(
        std::size_t probeIndex,
        std::uint64_t absoluteSamplePosition,
        RealtimeAnalysisPayloadKind payloadKind,
        std::uint32_t channelCount,
        std::span<const double> values) noexcept;
    [[nodiscard]] bool publishPartialSpectrum(
        std::size_t probeIndex,
        std::uint64_t absoluteSamplePosition,
        std::span<const talas::dsp::PartialSpectrumPoint> points) noexcept;
    void rejectPreparedRecord(std::size_t probeIndex) noexcept;
    // Single control/message-thread consumer per capture object.
    [[nodiscard]] std::optional<std::size_t> copyLatest(
        std::size_t probeIndex,
        std::span<double> destination) noexcept;
    [[nodiscard]] std::optional<RealtimeAnalysisRecordHeader>
    copyLatestRecord(
        std::size_t probeIndex,
        std::span<double> destination) noexcept;
    [[nodiscard]] RealtimeAnalysisCopyResult copyLatestRecordResult(
        std::size_t probeIndex,
        std::span<double> destination) noexcept;
    [[nodiscard]] RealtimePartialSpectrumCopyResult
    copyLatestPartialSpectrum(
        std::size_t probeIndex,
        std::span<talas::dsp::PartialSpectrumPoint> destination) noexcept;
    [[nodiscard]] std::uint64_t droppedBlockCount(std::size_t probeIndex) const noexcept;
    [[nodiscard]] std::uint64_t rejectedRecordCount(
        std::size_t probeIndex) const noexcept;

private:
    struct Probe;

    bool ready_ = false;
    std::size_t maxBlockFrames_ = 0;
    std::size_t queueCapacity_ = 0;
    std::vector<std::unique_ptr<Probe>> probes_;
    std::atomic<std::uint64_t> boundStageId_ = 0;
    std::atomic<std::uint64_t> bindingGeneration_ = 0;
};

struct PreparedScoreSourceAnalysis
{
    std::string clipId;
    std::uint32_t iterationOrdinal = 0;
    std::string soundDefinitionId;
    std::optional<std::string> sourceInstanceId;
    std::shared_ptr<RealtimeAnalysisCapture> capture;
};
using PreparedScoreSourceAnalysisSet = std::vector<PreparedScoreSourceAnalysis>;
}
