#include "talas/runtime/KernelAbi.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace
{
template <typename Type>
constexpr bool abiRecord = std::is_standard_layout_v<Type>
    && std::is_trivially_copyable_v<Type>;

static_assert(abiRecord<TalasKernelAbiStringViewV1>);
static_assert(abiRecord<TalasKernelAbiByteViewV1>);
static_assert(abiRecord<TalasKernelAbiCompatibilityV1>);
static_assert(abiRecord<TalasKernelAbiHostServicesV1>);
static_assert(abiRecord<TalasKernelAbiImmutableResourceViewV2>);
static_assert(abiRecord<TalasKernelAbiHostServicesV2>);
static_assert(abiRecord<TalasKernelAbiImmutableAudioStreamPrepareV3>);
static_assert(abiRecord<TalasKernelAbiImmutableAudioStreamViewV3>);
static_assert(abiRecord<TalasKernelAbiImmutableAudioStreamReadV3>);
static_assert(abiRecord<TalasKernelAbiImmutableAudioStreamReadResultV3>);
static_assert(abiRecord<TalasKernelAbiHostServicesV3>);
static_assert(abiRecord<TalasKernelAbiHostServicesV4>);
static_assert(abiRecord<TalasKernelAbiParameterV1>);
static_assert(abiRecord<TalasKernelAbiPrepareV1>);
static_assert(abiRecord<TalasKernelAbiSignalV1>);
static_assert(abiRecord<TalasKernelAbiOutputV1>);
static_assert(abiRecord<TalasKernelAbiEventV1>);
static_assert(abiRecord<TalasKernelAbiTransportV1>);
static_assert(abiRecord<TalasKernelAbiResetV1>);
static_assert(abiRecord<TalasKernelAbiProcessV1>);
static_assert(abiRecord<TalasKernelAbiDescriptorV1>);
static_assert(abiRecord<TalasKernelAbiModuleV1>);
static_assert(abiRecord<TalasKernelAbiCheckpointDescriptorV1>);
static_assert(abiRecord<TalasKernelAbiCheckpointExtensionV1>);

static_assert(sizeof(void*) == 8, "internal ABI v1 layout is frozen for 64-bit hosts");
#define TALAS_ABI_LAYOUT(type, size, alignment) \
    static_assert(sizeof(type) == size); \
    static_assert(alignof(type) == alignment);
#define TALAS_ABI_OFFSET(type, field, offset) \
    static_assert(offsetof(type, field) == offset);
#include "KernelAbiLayoutAssertions.inc"
#undef TALAS_ABI_OFFSET
#undef TALAS_ABI_LAYOUT

static_assert(std::is_same_v<
    decltype(TalasKernelAbiModuleV1::kernelCount),
    std::uint64_t>);
static_assert(std::is_same_v<
    decltype(TalasKernelAbiPrepareV1::maximumBlockFrames),
    std::uint64_t>);
static_assert(std::is_same_v<
    decltype(TalasKernelAbiSignalV1::sampleCount),
    std::uint64_t>);
static_assert(std::is_same_v<
    TalasKernelAbiProcessFlagsV1,
    std::uint32_t>);
static_assert(offsetof(TalasKernelAbiDescriptorV1, structSize) == 0);
static_assert(offsetof(TalasKernelAbiModuleV1, structSize) == 0);
static_assert(offsetof(TalasKernelAbiHostServicesV2, v1) == 0);
static_assert(offsetof(TalasKernelAbiHostServicesV3, v2) == 0);
static_assert(offsetof(TalasKernelAbiHostServicesV4, v3) == 0);
static_assert(sizeof(TalasKernelAbiHostServicesV1) == 56);
static_assert(TALAS_KERNEL_ABI_RESOURCE_TABLE == 1);
static_assert(TALAS_KERNEL_ABI_RESOURCE_BINARY == 2);
static_assert(TALAS_KERNEL_ABI_RESOURCE_ELEMENT_F64_LITTLE_ENDIAN == 1);
static_assert(TALAS_KERNEL_ABI_RESOURCE_ELEMENT_RAW_BYTES == 2);
static_assert(sizeof(TalasKernelAbiImmutableResourceViewV2) == 144);
static_assert(sizeof(TalasKernelAbiHostServicesV2) == 80);
static_assert(TALAS_KERNEL_API_VERSION_V3 == 3);
static_assert(TALAS_KERNEL_ABI_HOST_SERVICES_VERSION_V3 == 3);
static_assert(TALAS_KERNEL_ABI_AUDIO_STREAM_READ_COMPLETE == 1);
static_assert(TALAS_KERNEL_ABI_AUDIO_STREAM_READ_UNDERRUN == 2);
static_assert(TALAS_KERNEL_ABI_AUDIO_STREAM_READ_END_OF_STREAM == 3);
static_assert(sizeof(TalasKernelAbiHostServicesV3) == 112);
static_assert(TALAS_KERNEL_API_VERSION_V4 == 4);
static_assert(TALAS_KERNEL_ABI_HOST_SERVICES_VERSION_V4 == 4);
static_assert(sizeof(TalasKernelAbiHostServicesV4) == 128);
}

int main()
{
    const TalasKernelAbiModuleV1 module{};
    return module.reserved0 == 0 && module.reserved1 == 0 ? 0 : 1;
}
