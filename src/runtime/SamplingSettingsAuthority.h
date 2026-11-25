#pragma once

#include "talas/runtime/Runtime.h"

namespace talas::runtime::sampling_settings_authority
{
[[nodiscard]] bool hasExact(
    const RenderSettings& settings,
    const RenderSettings& authority);

[[nodiscard]] bool hasExactResources(
    const RenderSettings& settings,
    const std::vector<TableResource>& authorityTables,
    const std::vector<talas::core::ResourceSnapshotPtr>& authoritySnapshots);
}
