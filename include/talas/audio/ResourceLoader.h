#pragma once

#include "talas/core/SampleKeyRange.h"
#include "talas/core/SampleLoop.h"
#include "talas/core/SampleMarker.h"
#include "talas/core/SamplePitch.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace talas::audio
{
using ResourceLoadCancellationCheck = std::function<bool()>;

struct TableResourceLoadResult
{
    std::vector<double> samples;
    std::string error;
    std::string contentHash;
    bool cancelled = false;
    std::uint32_t sourceSampleRate = 0;
    std::optional<talas::core::SampleLoop> forwardLoop;
    std::optional<talas::core::SamplePitch> samplePitch;
    // The second channel of a stereo WAV table. Numeric tables remain mono.
    std::vector<double> rightSamples;
    // The notes and velocities a sample's INST or inst chunk gives.
    std::optional<talas::core::SampleKeyRange> keyRange;
    // Its named positions.
    std::vector<talas::core::SampleMarker> markers;

    [[nodiscard]] bool ok() const
    {
        return error.empty() && !cancelled;
    }
};

struct BinaryResourceLoadResult
{
    std::vector<std::uint8_t> bytes;
    std::string error;
    std::string contentHash;
    bool cancelled = false;

    [[nodiscard]] bool ok() const
    {
        return error.empty() && !cancelled;
    }
};

// Whether a path names a sample file, WAV or AIFF, by its extension.
[[nodiscard]] bool isSampleFilePath(const std::filesystem::path& path);

[[nodiscard]] TableResourceLoadResult loadTableResource(
    const std::filesystem::path& path,
    ResourceLoadCancellationCheck shouldCancel = {});
[[nodiscard]] BinaryResourceLoadResult loadBinaryResource(
    const std::filesystem::path& path,
    ResourceLoadCancellationCheck shouldCancel = {});
[[nodiscard]] BinaryResourceLoadResult loadPartialTrackResource(
    const std::filesystem::path& path,
    ResourceLoadCancellationCheck shouldCancel = {});
}
