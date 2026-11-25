#pragma once

#include "runtime/component_probe/ComponentBundleProbeProtocol.h"

#include <cstddef>
#include <span>

namespace talas::runtime::component_probe
{
[[nodiscard]] ComponentBundleProbeResultV1 runComponentBundleProbeLoaderV1(
    const ComponentBundleProbeRequestV1& request,
    const ComponentBundleProbeExpectedAbiV1& expectedAbi,
    std::span<const std::byte> entryBytes) noexcept;
}
