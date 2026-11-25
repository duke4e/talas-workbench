#pragma once

#include "talas/core/ResourceSnapshot.h"
#include "talas/project/ProjectSampleMaps.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace talas::project
{
struct ProjectDocument;
}

namespace talas::runtime
{
struct PreparedSampleMapRegion
{
    std::string regionId;
    std::string resourceId;
    std::string structuralSourcePath;
    std::uint8_t rootNote = 60;
    std::uint8_t lowNote = 0;
    std::uint8_t highNote = 127;
    std::uint8_t lowVelocity = 1;
    std::uint8_t highVelocity = 127;
    bool reverse = false;
    std::optional<talas::project::ProjectSampleLoop> sustainLoop;
    std::optional<talas::project::ProjectSampleLoop> releaseLoop;
    double sourceSampleRate = 0.0;
    std::uint64_t sourceFrameCount = 0;

    bool operator==(const PreparedSampleMapRegion&) const = default;
};

struct PreparedSampleMapPlayback
{
    std::string mapId;
    std::vector<PreparedSampleMapRegion> regions;

    bool operator==(const PreparedSampleMapPlayback&) const = default;
};

struct SampleMapPlaybackPreparationResult
{
    std::optional<PreparedSampleMapPlayback> playback;
    std::string errorPath;
    std::string error;

    [[nodiscard]] bool ok() const noexcept
    {
        return playback.has_value() && error.empty();
    }
};

struct SampleMapPlaybackSource
{
    struct ReadResult
    {
        double sample = 0.0;
        bool available = false;
    };

    using RestartFunction = bool (*)(
        void* context,
        double sourceFrame,
        double signedSourceFrameIncrement) noexcept;
    using ReadFunction = ReadResult (*)(
        void* context,
        double sourceFrame,
        double interpolationRightSourceFrame,
        double nextSourceFrame,
        double signedSourceFrameIncrement) noexcept;
    using RelocateFunction = bool (*)(
        void* context,
        double sourceFrame,
        double signedSourceFrameIncrement) noexcept;

    const double* samples = nullptr;
    std::size_t frameCount = 0;
    double sampleRate = 0.0;
    void* context = nullptr;
    RestartFunction restart = nullptr;
    ReadFunction read = nullptr;
    RelocateFunction relocate = nullptr;
};

struct SampleMapPlaybackControls
{
    double frequency = 440.0;
    double gate = 1.0;
    double velocity = 1.0;
    double scale = 1.0;
    double attackSeconds = 0.0;
    double releaseSeconds = 0.0;
};

struct SampleMapPlaybackLayerState
{
    std::size_t regionIndex = 0;
    double position = 0.0;
    double increment = 1.0;
    double direction = 1.0;
    double envelope = 1.0;
    double releaseStartEnvelope = 1.0;
    std::uint64_t envelopeFramesElapsed = 0;
    std::uint64_t attackFrames = 0;
    std::uint64_t releaseFrames = 0;
    bool released = false;
    bool active = false;
};

struct SampleMapPlaybackState
{
    std::array<
        SampleMapPlaybackLayerState,
        talas::project::maximumPreparedSampleMapLayers>
        layers{};
    std::size_t layerCount = 0;
    bool gateWasOpen = false;

    void reset() noexcept;
};

[[nodiscard]] SampleMapPlaybackPreparationResult
prepareSampleMapPlayback(
    const talas::project::ProjectDocument& document,
    std::string_view mapId,
    std::span<const talas::core::ResourceSnapshotPtr> resourceSnapshots,
    std::string_view structuralSourcePath);

[[nodiscard]] double renderSampleMapPlaybackFrame(
    const PreparedSampleMapPlayback& playback,
    SampleMapPlaybackState& state,
    std::span<const SampleMapPlaybackSource> sources,
    const SampleMapPlaybackControls& controls,
    double outputSampleRate) noexcept;
}
