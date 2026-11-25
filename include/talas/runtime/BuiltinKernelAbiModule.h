#pragma once

#include "talas/runtime/KernelAbi.h"

namespace talas::runtime
{
[[nodiscard]] const TalasKernelAbiModuleV1&
builtinKernelAbiModule() noexcept;

[[nodiscard]] const TalasKernelAbiCheckpointExtensionV1&
builtinKernelAbiCheckpointExtension() noexcept;
}
