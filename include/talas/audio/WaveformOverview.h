#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace talas::audio
{
inline constexpr std::size_t maximumWaveformOverviewBuckets = 16384;

using WaveformOverviewCancellationCheck = std::function<bool()>;

struct WaveformOverviewRequest
{
    std::string assetId;
    std::uint64_t generation = 1;
    std::size_t maximumBuckets = 2048;
    std::string contentHash;
};

struct WaveformChannelRange
{
    double minimum = 0.0;
    double maximum = 0.0;

    bool operator==(const WaveformChannelRange&) const = default;
};

struct WaveformOverviewBucket
{
    std::uint64_t firstFrame = 0;
    std::uint64_t endFrameExclusive = 0;
    std::vector<WaveformChannelRange> channels;

    bool operator==(const WaveformOverviewBucket&) const = default;
};

struct WaveformOverview
{
    std::string assetId;
    std::uint64_t generation = 1;
    std::string contentHash;
    std::uint32_t sampleRate = 0;
    std::uint32_t channelCount = 0;
    std::uint64_t frameCount = 0;
    std::vector<WaveformOverviewBucket> buckets;
};

struct WaveformOverviewResult
{
    std::shared_ptr<const WaveformOverview> overview;
    std::string error;
    bool cancelled = false;

    [[nodiscard]] bool ok() const noexcept
    {
        return overview != nullptr && error.empty() && !cancelled;
    }
};

[[nodiscard]] WaveformOverviewResult makeWaveformOverview(
    const WaveformOverviewRequest& request,
    std::uint32_t sampleRate,
    const std::vector<std::vector<double>>& channels,
    WaveformOverviewCancellationCheck shouldCancel = {});

[[nodiscard]] WaveformOverviewResult loadPcm16WaveformOverview(
    const std::filesystem::path& path,
    const WaveformOverviewRequest& request,
    WaveformOverviewCancellationCheck shouldCancel = {});

[[nodiscard]] bool waveformOverviewMatches(
    const WaveformOverview& overview,
    std::string_view assetId,
    std::uint64_t generation) noexcept;
}
