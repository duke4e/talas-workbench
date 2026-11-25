#include "talas/runtime/KernelAbi.h"

#include <stddef.h>
#include <stdint.h>

_Static_assert(TALAS_KERNEL_ABI_VERSION_V1 == UINT32_C(1),
    "the first internal ABI revision must be explicit");
_Static_assert(TALAS_KERNEL_API_VERSION_V1 == UINT32_C(1),
    "the first internal API revision must be explicit");
_Static_assert(TALAS_KERNEL_API_VERSION_V2 == UINT32_C(2),
    "the immutable-resource API revision must be explicit");
_Static_assert(TALAS_KERNEL_API_VERSION_V3 == UINT32_C(3),
    "the bounded Audio stream API revision must be explicit");
_Static_assert(TALAS_KERNEL_API_VERSION_V4 == UINT32_C(4),
    "the multiple-resource API revision must be explicit");
_Static_assert(TALAS_KERNEL_ABI_HOST_SERVICES_VERSION_V2 == UINT32_C(2),
    "the immutable-resource host-services revision must be explicit");
_Static_assert(TALAS_KERNEL_ABI_HOST_SERVICES_VERSION_V3 == UINT32_C(3),
    "the bounded Audio stream host-services revision must be explicit");
_Static_assert(TALAS_KERNEL_ABI_HOST_SERVICES_VERSION_V4 == UINT32_C(4),
    "the multiple-resource host-services revision must be explicit");
_Static_assert(TALAS_KERNEL_ABI_PARAMETER_RESOURCE == UINT32_C(4),
    "the immutable-resource parameter kind must be stable");
_Static_assert(TALAS_KERNEL_ABI_RESOURCE_TABLE == UINT32_C(1),
    "the immutable Table resource kind must remain stable");
_Static_assert(TALAS_KERNEL_ABI_RESOURCE_BINARY == UINT32_C(2),
    "the immutable Binary resource kind must be explicit");
_Static_assert(
    TALAS_KERNEL_ABI_RESOURCE_ELEMENT_F64_LITTLE_ENDIAN == UINT32_C(1),
    "the canonical Table element format must remain stable");
_Static_assert(TALAS_KERNEL_ABI_RESOURCE_ELEMENT_RAW_BYTES == UINT32_C(2),
    "the exact Binary octet format must be explicit");
_Static_assert(offsetof(TalasKernelAbiHostServicesV2, v1) == 0,
    "the v2 host services must retain the exact v1 prefix");
_Static_assert(offsetof(TalasKernelAbiHostServicesV3, v2) == 0,
    "the v3 host services must retain the exact v2 prefix");
_Static_assert(offsetof(TalasKernelAbiHostServicesV4, v3) == 0,
    "the v4 host services must retain the exact v3 prefix");
_Static_assert(TALAS_KERNEL_ABI_AUDIO_STREAM_READ_COMPLETE == UINT32_C(1),
    "the complete Audio read outcome must be stable");
_Static_assert(TALAS_KERNEL_ABI_AUDIO_STREAM_READ_UNDERRUN == UINT32_C(2),
    "the Audio underrun outcome must be explicit");
_Static_assert(TALAS_KERNEL_ABI_AUDIO_STREAM_READ_END_OF_STREAM == UINT32_C(3),
    "the Audio EOF outcome must be explicit");
_Static_assert(TALAS_KERNEL_ABI_SAMPLE_FORMAT_F64 == UINT64_C(1),
    "the initial ABI sample format must be one stable bit");
_Static_assert(TALAS_KERNEL_ABI_MAX_PROCESS_EVENTS_V1 == UINT64_C(65536),
    "the v1 process event budget must be explicit and bounded");
_Static_assert(
    (TALAS_KERNEL_ABI_PROCESS_TYPED_EVENTS
        & ~TALAS_KERNEL_ABI_PROCESS_KNOWN_MASK) == 0,
    "the typed-event process flag must be part of the frozen known mask");
_Static_assert(offsetof(TalasKernelAbiStringViewV1, data) == 0,
    "ABI string data must be the first field");
_Static_assert(
    sizeof(((TalasKernelAbiStringViewV1*)0)->byteCount) == sizeof(uint64_t),
    "ABI string sizes must not use platform size_t");
_Static_assert(
    sizeof(((TalasKernelAbiProcessV1*)0)->frameCount) == sizeof(uint64_t),
    "ABI frame counts must be fixed width");
_Static_assert(
    sizeof(((TalasKernelAbiEventV1*)0)->sequence) == sizeof(uint64_t),
    "ABI event sequence numbers must be fixed width");
_Static_assert(sizeof(void*) == 8,
    "internal ABI v1 layout is frozen for 64-bit hosts");
#define TALAS_ABI_LAYOUT(type, size, alignment) \
    _Static_assert(sizeof(type) == size, "ABI size drift"); \
    _Static_assert(_Alignof(type) == alignment, "ABI alignment drift");
#define TALAS_ABI_OFFSET(type, field, offset) \
    _Static_assert(offsetof(type, field) == offset, "ABI offset drift");
#include "KernelAbiLayoutAssertions.inc"
#undef TALAS_ABI_OFFSET
#undef TALAS_ABI_LAYOUT

static TalasKernelAbiStatusV1 createInstance(
    const TalasKernelAbiHostServicesV1* host,
    void** instance)
{
    (void)host;
    *instance = (void*)UINTPTR_MAX;
    return TALAS_KERNEL_ABI_STATUS_OK;
}

static TalasKernelAbiStatusV1 prepareInstance(
    void* instance,
    const TalasKernelAbiPrepareV1* prepare)
{
    (void)instance;
    (void)prepare;
    return TALAS_KERNEL_ABI_STATUS_OK;
}

static TalasKernelAbiStatusV1 resetInstance(
    void* instance,
    const TalasKernelAbiResetV1* reset)
{
    (void)instance;
    (void)reset;
    return TALAS_KERNEL_ABI_STATUS_OK;
}

static TalasKernelAbiStatusV1 processInstance(
    void* instance,
    const TalasKernelAbiProcessV1* process)
{
    (void)instance;
    (void)process;
    return TALAS_KERNEL_ABI_STATUS_OK;
}

static void destroyInstance(
    void* instance,
    const TalasKernelAbiHostServicesV1* host)
{
    (void)instance;
    (void)host;
}

int main(void)
{
    TalasKernelAbiDescriptorV1 descriptor = {0};
    descriptor.structSize = (uint32_t)sizeof(descriptor);
    descriptor.descriptorVersion = TALAS_KERNEL_ABI_DESCRIPTOR_VERSION_V1;
    descriptor.create = createInstance;
    descriptor.prepare = prepareInstance;
    descriptor.reset = resetInstance;
    descriptor.process = processInstance;
    descriptor.destroy = destroyInstance;
    return descriptor.reserved0 == 0 && descriptor.reserved1 == 0 ? 0 : 1;
}
