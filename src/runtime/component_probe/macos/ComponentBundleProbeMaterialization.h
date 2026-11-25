#pragma once

#include "runtime/component_probe/ComponentBundleProbeProtocol.h"

#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <span>

namespace talas::runtime::component_probe
{
[[nodiscard]] bool componentBundleProbeMaterializationPathV1(
    const ComponentBundleProbeRequestV1& request,
    std::array<char, PATH_MAX>& path) noexcept;

// Removes this process's materializations older than minimumAgeSeconds.
// A helper killed while an image initializer runs cannot clean up after
// itself, so each helper sweeps its own temporary root when it starts.
std::size_t sweepStaleComponentBundleProbeMaterializationsV1(
    std::uint64_t minimumAgeSeconds) noexcept;

[[nodiscard]] bool cleanupComponentBundleProbeMaterializationV1(
    const ComponentBundleProbeRequestV1& request,
    const ComponentBundleProbeExpectedAbiV1& expectedAbi,
    std::span<const std::byte> expectedEntryBytes) noexcept;
}
