#pragma once

#include "runtime/component_probe/ComponentBundleProbeProtocol.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace talas::runtime::component_probe
{
inline constexpr std::size_t maximumComponentBundleMachOLoadCommandsV1 = 512;
inline constexpr std::size_t maximumComponentBundleMachOLoadCommandBytesV1 =
    1024U * 1024U;
inline constexpr std::size_t maximumComponentBundleMachOSegmentsV1 = 64;
inline constexpr std::size_t maximumComponentBundleMachOSectionsV1 = 1024;
inline constexpr std::size_t maximumComponentBundleMachODependenciesV1 = 64;

enum class ComponentBundleMachOArchitectureV1 : std::uint8_t
{
    Unsupported,
    X86_64,
    Arm64,
};

enum class ComponentBundleMachOValidationCodeV1 : std::uint8_t
{
    Valid,
    EmptyImage,
    EntryBudgetExceeded,
    HeaderTruncated,
    MagicRejected,
    FatContainerRejected,
    ArchitectureMismatch,
    CpuSubtypeRejected,
    FileTypeRejected,
    HeaderFlagsRejected,
    LoadCommandBudgetExceeded,
    LoadCommandsTruncated,
    LoadCommandMalformed,
    LoadCommandUnknown,
    SegmentBudgetExceeded,
    SegmentMalformed,
    SegmentProtectionRejected,
    WritableExecutableSegment,
    SegmentFileRangeInvalid,
    SegmentFileRangeOverlap,
    SegmentVmRangeInvalid,
    SegmentVmRangeOverlap,
    SectionBudgetExceeded,
    SectionMalformed,
    LoaderPathRejected,
    NonSystemDependency,
    DependencyBudgetExceeded,
    InstallNameRejected,
    DylinkerRejected,
    LinkeditRangeInvalid,
    PlatformRejected,
    ExecutableTextMissing,
};

struct ComponentBundleMachOValidationResultV1
{
    ComponentBundleMachOValidationCodeV1 code =
        ComponentBundleMachOValidationCodeV1::EmptyImage;
    std::uint32_t commandIndex = 0;

    [[nodiscard]] constexpr bool ok() const noexcept
    {
        return code == ComponentBundleMachOValidationCodeV1::Valid;
    }
};

[[nodiscard]] constexpr ComponentBundleMachOArchitectureV1
currentComponentBundleMachOArchitectureV1() noexcept
{
#if defined(__arm64__) || defined(__aarch64__)
    return ComponentBundleMachOArchitectureV1::Arm64;
#elif defined(__x86_64__)
    return ComponentBundleMachOArchitectureV1::X86_64;
#else
    return ComponentBundleMachOArchitectureV1::Unsupported;
#endif
}

[[nodiscard]] ComponentBundleMachOValidationResultV1
validateComponentBundleMachOV1(
    std::span<const std::uint8_t> image,
    ComponentBundleMachOArchitectureV1 expectedArchitecture) noexcept;
}
