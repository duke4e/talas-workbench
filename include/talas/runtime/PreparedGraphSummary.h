#pragma once

#include "talas/core/ResourceSnapshot.h"
#include "talas/dsp/SpectralWindow.h"
#include "talas/runtime/Runtime.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace talas::runtime
{
enum class PreparedOutputLayout : std::uint8_t
{
    None,
    Mono,
    Stereo,
    Discrete,
};

struct PreparedOutputSummary
{
    std::size_t projectOrdinal = 0;
    std::string nodeId;
    std::string portId;
    RuntimeScopeId scopeId = globalRuntimeScope;
    std::string structuralSourcePath;
    talas::core::PortType type = talas::core::PortType::Audio;
};

struct PreparedLatencyCompensationSummary
{
    std::string nodeId;
    std::string inputPortId;
    RuntimeScopeId scopeId = globalRuntimeScope;
    std::string structuralSourcePath;
    std::size_t uncompensatedFrames = 0;
    std::size_t insertedFrames = 0;
    std::size_t compensatedFrames = 0;
};

struct PreparedLatencyWarningSummary
{
    LatencyPlanWarningKind kind =
        LatencyPlanWarningKind::UncompensatedUnknownLatency;
    std::string nodeId;
    RuntimeScopeId scopeId = globalRuntimeScope;
    std::string structuralSourcePath;
    std::string message;
};

struct PreparedLatencySummary
{
    std::optional<std::size_t> criticalPathFrames;
    std::optional<std::size_t> terminalOutputFrames;
    std::vector<PreparedLatencyCompensationSummary> compensations;
    std::vector<PreparedLatencyWarningSummary> warnings;
};

enum class PreparedSpectralFormat : std::uint8_t
{
    ComplexSpectrumV1,
    PackedRealAudioV1,
    PartialSpectrumV1,
    LegacyPartialTriples,
    // The stereo FFT format on two Audio channels.
    TalasStereoAudioV1,
};

enum class PreparedSpectralRole : std::uint8_t
{
    Forward,
    Inverse,
    Analysis,
    CompatibilityHold,
};

struct PreparedSpectralNodeSummary
{
    std::string nodeId;
    std::string typeId;
    RuntimeScopeId scopeId = globalRuntimeScope;
    std::string structuralSourcePath;
    PreparedSpectralFormat format =
        PreparedSpectralFormat::ComplexSpectrumV1;
    PreparedSpectralRole role = PreparedSpectralRole::Forward;
    std::optional<std::size_t> frameSize;
    std::optional<std::size_t> hopSize;
    std::optional<talas::dsp::SpectralWindowKind> window;
    double cadenceHz = 0.0;
    std::optional<std::size_t> intrinsicLatencyFrames;
    std::optional<std::size_t> cumulativeInputLatencyFrames;
    std::optional<std::size_t> cumulativeOutputLatencyFrames;
    std::vector<std::string> limitationIds;
    std::vector<std::string> limitationSummaries;
};

enum class PreparedResourceHealth : std::uint8_t
{
    Verified,
    Compatibility,
    Invalid,
};

struct PreparedResourceSummary
{
    std::string id;
    talas::core::ResourceSnapshotKind kind =
        talas::core::ResourceSnapshotKind::Table;
    std::uint64_t generation = 0;
    std::string contentHash;
    PreparedResourceHealth health = PreparedResourceHealth::Invalid;
    std::uint32_t channelCount = 0;
    double sampleRate = 0.0;
    std::uint64_t frameCount = 0;
    std::string encoding;
};

enum class PreparedStreamRole : std::uint8_t
{
    Playback,
    CachePrewarm,
};

struct PreparedStreamContractSummary
{
    std::string nodeId;
    std::string typeId;
    RuntimeScopeId scopeId = globalRuntimeScope;
    std::string structuralSourcePath;
    std::string resourceId;
    PreparedStreamRole role = PreparedStreamRole::Playback;
    std::vector<std::string> limitationIds;
    std::vector<std::string> limitationSummaries;
};

struct PreparedMidiMapperSummary
{
    std::string nodeId;
    RuntimeScopeId scopeId = globalRuntimeScope;
    std::string structuralSourcePath;
    PreparedMidiMapperConfiguration configuration;
    bool polyphonicInput = false;
};

struct PreparedProcessorPlacementIdentity
{
    std::string nodeId;
    std::string typeId;
    RuntimeScopeId scopeId = globalRuntimeScope;
    std::string structuralSourcePath;

    bool operator==(const PreparedProcessorPlacementIdentity&) const = default;
};

struct PreparedProcessorPlacementDomainSummary
{
    LogicalProcessorId processorId = hostLogicalProcessor;
    std::vector<PreparedProcessorPlacementIdentity> steps;
    std::vector<PreparedProcessorPlacementIdentity> boundaries;

    bool operator==(const PreparedProcessorPlacementDomainSummary&) const = default;
};

struct PreparedProcessorPlacementTransferSummary
{
    PreparedProcessorPlacementIdentity source;
    std::string sourcePortId;
    PreparedProcessorPlacementIdentity target;
    std::string targetPortId;
    LogicalProcessorId sourceProcessorId = hostLogicalProcessor;
    LogicalProcessorId targetProcessorId = hostLogicalProcessor;

    bool operator==(const PreparedProcessorPlacementTransferSummary&) const = default;
};

struct PreparedDiskRecorderCaptureSummary
{
    std::string nodeId;
    RuntimeScopeId scopeId = globalRuntimeScope;
    std::string structuralSourcePath;
    std::size_t captureBufferIndex = 0;
    std::optional<std::size_t> triggerBufferIndex;
    std::optional<std::size_t> captureDurationBufferIndex;
    std::string destinationNameHint;
    std::string format;
    std::string wordSize;
    std::string channels;
    double trigger = 0.0;
    double captureDurationSeconds = 0.0;
    bool gated = false;
    DiskRecorderWriteSupport writeSupport =
        DiskRecorderWriteSupport::UnsupportedFormat;

    bool operator==(const PreparedDiskRecorderCaptureSummary&) const = default;
};

struct PreparedGraphSummary
{
    std::uint64_t stageId = 0;
    double sampleRate = 0.0;
    std::size_t maxBlockFrames = 0;
    PreparedOutputLayout outputLayout = PreparedOutputLayout::None;
    std::vector<PreparedOutputSummary> outputs;
    PreparedLatencySummary latency;
    std::vector<PreparedSpectralNodeSummary> spectralNodes;
    std::vector<PreparedResourceSummary> resources;
    std::vector<PreparedStreamContractSummary> streams;
    std::vector<PreparedMidiMapperSummary> midiMappers;
    std::vector<PreparedProcessorPlacementDomainSummary>
        processorPlacements;
    std::vector<PreparedProcessorPlacementTransferSummary>
        processorTransfers;
    std::optional<PreparedDiskRecorderCaptureSummary>
        diskRecorderCapture;
    // The DiskRecorders within the Sound, in the
    // plan's order.
    std::vector<PreparedDiskRecorderCaptureSummary> diskRecorderTaps;
};

struct PreparedGraphSummaryResult
{
    std::optional<PreparedGraphSummary> summary;
    std::string error;

    [[nodiscard]] bool ok() const noexcept
    {
        return summary.has_value() && error.empty();
    }
};

[[nodiscard]] PreparedGraphSummaryResult
makePreparedGraphSummary(const PreparedRuntimeGraph& graph);

[[nodiscard]] std::string_view
preparedOutputLayoutName(PreparedOutputLayout layout) noexcept;
[[nodiscard]] std::string_view
preparedSpectralFormatName(PreparedSpectralFormat format) noexcept;
[[nodiscard]] std::string_view
preparedSpectralRoleName(PreparedSpectralRole role) noexcept;
[[nodiscard]] std::string_view
preparedResourceKindName(talas::core::ResourceSnapshotKind kind) noexcept;
[[nodiscard]] std::string_view
preparedResourceHealthName(PreparedResourceHealth health) noexcept;
[[nodiscard]] std::string_view
preparedStreamRoleName(PreparedStreamRole role) noexcept;
[[nodiscard]] std::string_view
preparedMidiMapperModeName(PreparedMidiMapperMode mode) noexcept;
}
