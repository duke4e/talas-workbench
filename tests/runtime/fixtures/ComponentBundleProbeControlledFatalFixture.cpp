#include "talas/runtime/KernelAbi.h"

// Original Talas Apache-2.0 test module. It is built only under
// TALAS_BUILD_TESTS and always reports one controlled entry failure.

extern "C" __attribute__((visibility("default")))
TalasKernelAbiStatusV1 talas_component_bundle_entry_v1(
    const TalasKernelAbiHostServicesV1*,
    const TalasKernelAbiModuleV1** output)
{
    if (output != nullptr)
        *output = nullptr;
    return TALAS_KERNEL_ABI_STATUS_FATAL;
}
