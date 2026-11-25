#include "runtime/component_probe/ComponentBundleMachOValidation.h"

#include <array>
#include <limits>
#include <string_view>

namespace talas::runtime::component_probe
{
namespace
{
constexpr std::uint32_t machMagic64 = 0xfeedfacf;
constexpr std::uint32_t cpuTypeX86_64 = 0x01000007;
constexpr std::uint32_t cpuTypeArm64 = 0x0100000c;
constexpr std::uint32_t cpuSubtypeX86_64All = 3;
constexpr std::uint32_t cpuSubtypeX86_64Haswell = 8;
constexpr std::uint32_t cpuSubtypeArm64All = 0;
constexpr std::uint32_t cpuSubtypeArm64V8 = 1;
constexpr std::uint32_t mhBundle = 8;
constexpr std::uint32_t mhNoUndefined = 0x1;
constexpr std::uint32_t mhDyldLink = 0x4;
constexpr std::uint32_t mhBindAtLoad = 0x8;
constexpr std::uint32_t mhTwoLevel = 0x80;
constexpr std::uint32_t mhForceFlat = 0x100;
constexpr std::uint32_t mhNoMultipleDefinitions = 0x200;
constexpr std::uint32_t mhNoFixPrebinding = 0x400;
constexpr std::uint32_t mhPrebindable = 0x800;
constexpr std::uint32_t mhAllModulesBound = 0x1000;
constexpr std::uint32_t mhSubsectionsViaSymbols = 0x2000;
constexpr std::uint32_t mhWeakDefines = 0x8000;
constexpr std::uint32_t mhBindsToWeak = 0x10000;
constexpr std::uint32_t mhAllowStackExecution = 0x20000;
constexpr std::uint32_t mhNoReexportedDylibs = 0x100000;
constexpr std::uint32_t mhDeadStrippableDylib = 0x400000;
constexpr std::uint32_t mhHasThreadLocalDescriptors = 0x800000;
constexpr std::uint32_t mhNoHeapExecution = 0x1000000;
constexpr std::uint32_t mhAppExtensionSafe = 0x2000000;
constexpr std::uint32_t acceptedHeaderFlags = mhNoUndefined | mhDyldLink
    | mhBindAtLoad | mhTwoLevel | mhNoMultipleDefinitions
    | mhNoFixPrebinding | mhPrebindable | mhAllModulesBound
    | mhSubsectionsViaSymbols | mhWeakDefines | mhBindsToWeak
    | mhNoReexportedDylibs | mhDeadStrippableDylib
    | mhHasThreadLocalDescriptors | mhNoHeapExecution
    | mhAppExtensionSafe;

constexpr std::uint32_t lcSegment64 = 0x19;
constexpr std::uint32_t lcSymtab = 0x2;
constexpr std::uint32_t lcDysymtab = 0xb;
constexpr std::uint32_t lcLoadDylib = 0xc;
constexpr std::uint32_t lcIdDylib = 0xd;
constexpr std::uint32_t lcLoadDylinker = 0xe;
constexpr std::uint32_t lcIdDylinker = 0xf;
constexpr std::uint32_t lcLoadWeakDylib = 0x80000018;
constexpr std::uint32_t lcUuid = 0x1b;
constexpr std::uint32_t lcRpath = 0x8000001c;
constexpr std::uint32_t lcCodeSignature = 0x1d;
constexpr std::uint32_t lcSegmentSplitInfo = 0x1e;
constexpr std::uint32_t lcReexportDylib = 0x8000001f;
constexpr std::uint32_t lcLazyLoadDylib = 0x20;
constexpr std::uint32_t lcLoadUpwardDylib = 0x80000023;
constexpr std::uint32_t lcVersionMinMacOS = 0x24;
constexpr std::uint32_t lcFunctionStarts = 0x26;
constexpr std::uint32_t lcDyldEnvironment = 0x27;
constexpr std::uint32_t lcMain = 0x80000028;
constexpr std::uint32_t lcDataInCode = 0x29;
constexpr std::uint32_t lcSourceVersion = 0x2a;
constexpr std::uint32_t lcDylibCodeSignDrs = 0x2b;
constexpr std::uint32_t lcLinkerOptimizationHint = 0x2e;
constexpr std::uint32_t lcBuildVersion = 0x32;
constexpr std::uint32_t lcDyldExportsTrie = 0x80000033;
constexpr std::uint32_t lcDyldChainedFixups = 0x80000034;
constexpr std::uint32_t lcAtomInfo = 0x36;
constexpr std::uint32_t lcFunctionVariants = 0x37;
constexpr std::uint32_t lcFunctionVariantFixups = 0x38;
constexpr std::uint32_t lcLazyLoadDylibInfo = 0x3a;
constexpr std::uint32_t lcDyldInfoOnly = 0x80000022;

constexpr std::uint32_t vmRead = 1;
constexpr std::uint32_t vmWrite = 2;
constexpr std::uint32_t vmExecute = 4;

struct Range
{
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
};

[[nodiscard]] constexpr ComponentBundleMachOValidationResultV1 rejected(
    ComponentBundleMachOValidationCodeV1 code,
    std::uint32_t commandIndex = 0) noexcept
{
    return {code, commandIndex};
}

[[nodiscard]] constexpr bool rangeValid(
    std::uint64_t offset,
    std::uint64_t size,
    std::uint64_t limit) noexcept
{
    return offset <= limit && size <= limit - offset;
}

[[nodiscard]] constexpr bool rangesOverlap(
    Range left,
    Range right) noexcept
{
    if (left.size == 0 || right.size == 0)
        return false;
    return left.offset < right.offset + right.size
        && right.offset < left.offset + left.size;
}

[[nodiscard]] std::uint32_t readU32(
    std::span<const std::uint8_t> bytes,
    std::size_t offset) noexcept
{
    return static_cast<std::uint32_t>(bytes[offset])
        | (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U)
        | (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U)
        | (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

[[nodiscard]] std::uint64_t readU64(
    std::span<const std::uint8_t> bytes,
    std::size_t offset) noexcept
{
    return static_cast<std::uint64_t>(readU32(bytes, offset))
        | (static_cast<std::uint64_t>(readU32(bytes, offset + 4U)) << 32U);
}

[[nodiscard]] bool fixedNameValid(
    std::span<const std::uint8_t> bytes) noexcept
{
    bool sawCharacter = false;
    bool sawTerminator = false;
    for (const auto byte : bytes)
    {
        if (sawTerminator)
        {
            if (byte != 0)
                return false;
            continue;
        }
        if (byte == 0)
        {
            sawTerminator = true;
            continue;
        }
        if (byte < 0x20U || byte > 0x7eU)
            return false;
        sawCharacter = true;
    }
    return sawCharacter;
}

[[nodiscard]] std::string_view fixedName(
    std::span<const std::uint8_t> bytes) noexcept
{
    std::size_t size = 0;
    while (size < bytes.size() && bytes[size] != 0)
        ++size;
    return {
        reinterpret_cast<const char*>(bytes.data()),
        size,
    };
}

[[nodiscard]] bool commandText(
    std::span<const std::uint8_t> command,
    std::size_t minimumOffset,
    std::string_view& result) noexcept
{
    if (command.size() < 12U)
        return false;
    const auto textOffset = readU32(command, 8);
    if (textOffset != minimumOffset || textOffset >= command.size())
        return false;
    std::size_t end = textOffset;
    while (end < command.size() && command[end] != 0)
    {
        if (command[end] < 0x20U || command[end] > 0x7eU)
            return false;
        ++end;
    }
    if (end == textOffset || end == command.size())
        return false;
    for (std::size_t padding = end + 1U;
         padding < command.size();
         ++padding)
    {
        if (command[padding] != 0)
            return false;
    }
    result = {
        reinterpret_cast<const char*>(command.data() + textOffset),
        end - textOffset,
    };
    return true;
}

[[nodiscard]] bool systemDependency(std::string_view path) noexcept
{
    if ((!path.starts_with("/usr/lib/")
            && !path.starts_with("/System/Library/Frameworks/"))
        || path.ends_with('/') || path.find("//") != std::string_view::npos)
    {
        return false;
    }
    std::size_t segmentStart = 1;
    while (segmentStart < path.size())
    {
        const auto separator = path.find('/', segmentStart);
        const auto segment = path.substr(
            segmentStart,
            separator == std::string_view::npos
                ? path.size() - segmentStart
                : separator - segmentStart);
        if (segment.empty() || segment == "." || segment == "..")
            return false;
        if (separator == std::string_view::npos)
            break;
        segmentStart = separator + 1U;
    }
    return true;
}

[[nodiscard]] bool linkeditRangeValid(
    std::span<const std::uint8_t> command,
    std::uint64_t imageBytes) noexcept
{
    return command.size() == 16U
        && rangeValid(
            readU32(command, 8),
            readU32(command, 12),
            imageBytes);
}

[[nodiscard]] bool dyldInfoRangesValid(
    std::span<const std::uint8_t> command,
    std::uint64_t imageBytes) noexcept
{
    if (command.size() != 48U)
        return false;
    for (std::size_t offset = 8; offset < 48; offset += 8)
    {
        if (!rangeValid(
                readU32(command, offset),
                readU32(command, offset + 4U),
                imageBytes))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool symtabRangesValid(
    std::span<const std::uint8_t> command,
    std::uint64_t imageBytes) noexcept
{
    if (command.size() != 24U)
        return false;
    const auto symbols = readU32(command, 12);
    if (symbols > imageBytes / 16U)
        return false;
    return rangeValid(
            readU32(command, 8),
            static_cast<std::uint64_t>(symbols) * 16U,
            imageBytes)
        && rangeValid(
            readU32(command, 16),
            readU32(command, 20),
            imageBytes);
}

[[nodiscard]] bool dysymtabRangesValid(
    std::span<const std::uint8_t> command,
    std::uint64_t imageBytes) noexcept
{
    if (command.size() != 80U)
        return false;
    constexpr std::array<std::pair<std::size_t, std::uint64_t>, 6> ranges{{
        {32, 8},
        {40, 56},
        {48, 4},
        {56, 4},
        {64, 8},
        {72, 8},
    }};
    for (const auto [offset, stride] : ranges)
    {
        const auto count = readU32(command, offset + 4U);
        if (count > imageBytes / stride
            || !rangeValid(
                readU32(command, offset),
                static_cast<std::uint64_t>(count) * stride,
                imageBytes))
        {
            return false;
        }
    }
    return true;
}
}

ComponentBundleMachOValidationResultV1 validateComponentBundleMachOV1(
    std::span<const std::uint8_t> image,
    ComponentBundleMachOArchitectureV1 expectedArchitecture) noexcept
{
    if (image.empty())
        return rejected(ComponentBundleMachOValidationCodeV1::EmptyImage);
    if (image.size() > maximumComponentBundleProbeEntryBytesV1)
    {
        return rejected(
            ComponentBundleMachOValidationCodeV1::EntryBudgetExceeded);
    }
    if (image.size() < 4U)
        return rejected(ComponentBundleMachOValidationCodeV1::HeaderTruncated);

    const auto magic = readU32(image, 0);
    if (magic == 0xcafebabe || magic == 0xbebafeca
        || magic == 0xcafebabf || magic == 0xbfbafeca)
    {
        return rejected(
            ComponentBundleMachOValidationCodeV1::FatContainerRejected);
    }
    if (magic != machMagic64)
        return rejected(ComponentBundleMachOValidationCodeV1::MagicRejected);
    if (image.size() < 32U)
        return rejected(ComponentBundleMachOValidationCodeV1::HeaderTruncated);

    const auto cpuType = readU32(image, 4);
    const auto cpuSubtype = readU32(image, 8) & 0x00ffffffU;
    const auto expectedCpu = expectedArchitecture
            == ComponentBundleMachOArchitectureV1::Arm64
        ? cpuTypeArm64
        : expectedArchitecture == ComponentBundleMachOArchitectureV1::X86_64
        ? cpuTypeX86_64
        : 0U;
    if (expectedCpu == 0 || cpuType != expectedCpu)
    {
        return rejected(
            ComponentBundleMachOValidationCodeV1::ArchitectureMismatch);
    }
    const auto subtypeAccepted = expectedArchitecture
            == ComponentBundleMachOArchitectureV1::Arm64
        ? cpuSubtype == cpuSubtypeArm64All || cpuSubtype == cpuSubtypeArm64V8
        : cpuSubtype == cpuSubtypeX86_64All
            || cpuSubtype == cpuSubtypeX86_64Haswell;
    if (!subtypeAccepted)
    {
        return rejected(
            ComponentBundleMachOValidationCodeV1::CpuSubtypeRejected);
    }
    if (readU32(image, 12) != mhBundle)
        return rejected(ComponentBundleMachOValidationCodeV1::FileTypeRejected);
    const auto flags = readU32(image, 24);
    if ((flags & (mhNoUndefined | mhDyldLink))
            != (mhNoUndefined | mhDyldLink)
        || (flags & ~acceptedHeaderFlags) != 0
        || (flags & (mhForceFlat | mhAllowStackExecution)) != 0
        || readU32(image, 28) != 0)
    {
        return rejected(
            ComponentBundleMachOValidationCodeV1::HeaderFlagsRejected);
    }

    const auto commandCount = readU32(image, 16);
    const auto commandBytes = readU32(image, 20);
    if (commandCount == 0
        || commandCount > maximumComponentBundleMachOLoadCommandsV1
        || commandBytes > maximumComponentBundleMachOLoadCommandBytesV1)
    {
        return rejected(
            ComponentBundleMachOValidationCodeV1::LoadCommandBudgetExceeded);
    }
    if (commandCount > commandBytes / 8U)
    {
        return rejected(
            ComponentBundleMachOValidationCodeV1::LoadCommandMalformed);
    }
    if (!rangeValid(32, commandBytes, image.size()))
    {
        return rejected(
            ComponentBundleMachOValidationCodeV1::LoadCommandsTruncated);
    }

    std::array<Range, maximumComponentBundleMachOSegmentsV1> fileRanges{};
    std::array<Range, maximumComponentBundleMachOSegmentsV1> vmRanges{};
    std::array<Range, maximumComponentBundleMachOSectionsV1>
        sectionFileRanges{};
    std::array<Range, maximumComponentBundleMachOSectionsV1>
        sectionVmRanges{};
    std::size_t segmentCount = 0;
    std::size_t sectionCount = 0;
    std::size_t processedSectionCount = 0;
    std::size_t dependencyCount = 0;
    bool executableText = false;
    std::size_t cursor = 32;
    const auto commandEnd = cursor + commandBytes;
    for (std::uint32_t index = 0; index < commandCount; ++index)
    {
        if (cursor > commandEnd || commandEnd - cursor < 8U)
        {
            return rejected(
                ComponentBundleMachOValidationCodeV1::LoadCommandMalformed,
                index);
        }
        const auto commandKind = readU32(image, cursor);
        const auto size = readU32(image, cursor + 4U);
        if (size < 8U || (size & 7U) != 0
            || size > commandEnd - cursor)
        {
            return rejected(
                ComponentBundleMachOValidationCodeV1::LoadCommandMalformed,
                index);
        }
        const auto command = image.subspan(cursor, size);
        if (commandKind == lcSegment64)
        {
            if (segmentCount >= maximumComponentBundleMachOSegmentsV1)
            {
                return rejected(
                    ComponentBundleMachOValidationCodeV1::
                        SegmentBudgetExceeded,
                    index);
            }
            if (size < 72U || (size - 72U) % 80U != 0
                || !fixedNameValid(command.subspan(8, 16)))
            {
                return rejected(
                    ComponentBundleMachOValidationCodeV1::SegmentMalformed,
                    index);
            }
            const auto sections = readU32(command, 64);
            if (sections != (size - 72U) / 80U)
            {
                return rejected(
                    ComponentBundleMachOValidationCodeV1::SegmentMalformed,
                    index);
            }
            if (sections > maximumComponentBundleMachOSectionsV1 - sectionCount)
            {
                return rejected(
                    ComponentBundleMachOValidationCodeV1::SectionBudgetExceeded,
                    index);
            }
            sectionCount += sections;
            const auto vmRange = Range{readU64(command, 24), readU64(command, 32)};
            const auto fileRange = Range{readU64(command, 40), readU64(command, 48)};
            const auto maximumProtection = readU32(command, 56);
            const auto initialProtection = readU32(command, 60);
            if ((maximumProtection & ~(vmRead | vmWrite | vmExecute)) != 0
                || (initialProtection & ~(vmRead | vmWrite | vmExecute)) != 0
                || (initialProtection & ~maximumProtection) != 0)
            {
                return rejected(
                    ComponentBundleMachOValidationCodeV1::
                        SegmentProtectionRejected,
                    index);
            }
            if (((maximumProtection | initialProtection) & vmWrite) != 0
                && ((maximumProtection | initialProtection) & vmExecute) != 0)
            {
                return rejected(
                    ComponentBundleMachOValidationCodeV1::
                        WritableExecutableSegment,
                    index);
            }
            if (!rangeValid(fileRange.offset, fileRange.size, image.size())
                || fileRange.size > vmRange.size)
            {
                return rejected(
                    ComponentBundleMachOValidationCodeV1::
                        SegmentFileRangeInvalid,
                    index);
            }
            if (!rangeValid(
                    vmRange.offset,
                    vmRange.size,
                    std::numeric_limits<std::uint64_t>::max()))
            {
                return rejected(
                    ComponentBundleMachOValidationCodeV1::
                        SegmentVmRangeInvalid,
                    index);
            }
            for (std::size_t previous = 0; previous < segmentCount; ++previous)
            {
                if (rangesOverlap(fileRanges[previous], fileRange))
                {
                    return rejected(
                        ComponentBundleMachOValidationCodeV1::
                            SegmentFileRangeOverlap,
                        index);
                }
                if (rangesOverlap(vmRanges[previous], vmRange))
                {
                    return rejected(
                        ComponentBundleMachOValidationCodeV1::
                            SegmentVmRangeOverlap,
                        index);
                }
            }
            fileRanges[segmentCount] = fileRange;
            vmRanges[segmentCount] = vmRange;
            ++segmentCount;
            const auto segmentName = fixedName(command.subspan(8, 16));
            if (segmentName == "__TEXT" && fileRange.offset == 0
                && fileRange.size >= commandEnd
                && (initialProtection & vmExecute) != 0
                && (initialProtection & vmWrite) == 0)
            {
                executableText = true;
            }
            for (std::size_t section = 0; section < sections; ++section)
            {
                const auto offset = 72U + section * 80U;
                const auto record = command.subspan(offset, 80);
                if (!fixedNameValid(record.subspan(0, 16))
                    || !fixedNameValid(record.subspan(16, 16))
                    || fixedName(record.subspan(16, 16)) != segmentName
                    || readU32(record, 52) > 31U)
                {
                    return rejected(
                        ComponentBundleMachOValidationCodeV1::SectionMalformed,
                        index);
                }
                const auto address = readU64(record, 32);
                const auto byteCount = readU64(record, 40);
                if (!rangeValid(address, byteCount, vmRange.offset + vmRange.size)
                    || address < vmRange.offset)
                {
                    return rejected(
                        ComponentBundleMachOValidationCodeV1::SectionMalformed,
                        index);
                }
                const auto sectionType = readU32(record, 64) & 0xffU;
                const auto zeroFill = sectionType == 1U
                    || sectionType == 0xcU || sectionType == 0x12U;
                auto sectionFileRange = Range{};
                if (!zeroFill)
                {
                    const auto fileOffset = readU32(record, 48);
                    if (!rangeValid(fileOffset, byteCount, image.size())
                        || fileOffset < fileRange.offset
                        || (byteCount != 0 && fileOffset < commandEnd)
                        || byteCount
                            > fileRange.size - (fileOffset - fileRange.offset))
                    {
                        return rejected(
                            ComponentBundleMachOValidationCodeV1::
                                SectionMalformed,
                            index);
                    }
                    sectionFileRange = {fileOffset, byteCount};
                }
                const auto relocations = readU32(record, 60);
                if (relocations > image.size() / 8U
                    || !rangeValid(
                        readU32(record, 56),
                        static_cast<std::uint64_t>(relocations) * 8U,
                        image.size()))
                {
                    return rejected(
                        ComponentBundleMachOValidationCodeV1::SectionMalformed,
                        index);
                }
                const auto sectionVmRange = Range{address, byteCount};
                for (std::size_t previous = 0;
                     previous < processedSectionCount;
                     ++previous)
                {
                    if (rangesOverlap(
                            sectionFileRanges[previous],
                            sectionFileRange)
                        || rangesOverlap(
                            sectionVmRanges[previous],
                            sectionVmRange))
                    {
                        return rejected(
                            ComponentBundleMachOValidationCodeV1::
                                SectionMalformed,
                            index);
                    }
                }
                sectionFileRanges[processedSectionCount] = sectionFileRange;
                sectionVmRanges[processedSectionCount] = sectionVmRange;
                ++processedSectionCount;
            }
        }
        else if (commandKind == lcLoadDylib
            || commandKind == lcLoadWeakDylib
            || commandKind == lcReexportDylib
            || commandKind == lcLazyLoadDylib
            || commandKind == lcLoadUpwardDylib)
        {
            if (dependencyCount >= maximumComponentBundleMachODependenciesV1)
            {
                return rejected(
                    ComponentBundleMachOValidationCodeV1::
                        DependencyBudgetExceeded,
                    index);
            }
            ++dependencyCount;
            std::string_view path;
            if (command.size() < 24U || !commandText(command, 24, path))
            {
                return rejected(
                    ComponentBundleMachOValidationCodeV1::LoadCommandMalformed,
                    index);
            }
            if (path.starts_with("@"))
            {
                return rejected(
                    ComponentBundleMachOValidationCodeV1::LoaderPathRejected,
                    index);
            }
            if (!systemDependency(path))
            {
                return rejected(
                    ComponentBundleMachOValidationCodeV1::NonSystemDependency,
                    index);
            }
        }
        else if (commandKind == lcIdDylib)
        {
            return rejected(
                ComponentBundleMachOValidationCodeV1::InstallNameRejected,
                index);
        }
        else if (commandKind == lcRpath)
        {
            return rejected(
                ComponentBundleMachOValidationCodeV1::LoaderPathRejected,
                index);
        }
        else if (commandKind == lcLoadDylinker
            || commandKind == lcIdDylinker
            || commandKind == lcDyldEnvironment || commandKind == lcMain)
        {
            return rejected(
                ComponentBundleMachOValidationCodeV1::DylinkerRejected,
                index);
        }
        else if (commandKind == lcUuid)
        {
            if (size != 24U)
            {
                return rejected(
                    ComponentBundleMachOValidationCodeV1::LoadCommandMalformed,
                    index);
            }
        }
        else if (commandKind == lcVersionMinMacOS)
        {
            if (size != 16U)
            {
                return rejected(
                    ComponentBundleMachOValidationCodeV1::LoadCommandMalformed,
                    index);
            }
        }
        else if (commandKind == lcBuildVersion)
        {
            if (size < 24U || readU32(command, 8) != 1U)
            {
                return rejected(
                    ComponentBundleMachOValidationCodeV1::PlatformRejected,
                    index);
            }
            const auto tools = readU32(command, 20);
            if (tools > 32U || size != 24U + tools * 8U)
            {
                return rejected(
                    ComponentBundleMachOValidationCodeV1::LoadCommandMalformed,
                    index);
            }
        }
        else if (commandKind == lcSourceVersion)
        {
            if (size != 16U)
            {
                return rejected(
                    ComponentBundleMachOValidationCodeV1::LoadCommandMalformed,
                    index);
            }
        }
        else if (commandKind == lcSymtab)
        {
            if (!symtabRangesValid(command, image.size()))
            {
                return rejected(
                    ComponentBundleMachOValidationCodeV1::LinkeditRangeInvalid,
                    index);
            }
        }
        else if (commandKind == lcDysymtab)
        {
            if (!dysymtabRangesValid(command, image.size()))
            {
                return rejected(
                    ComponentBundleMachOValidationCodeV1::LinkeditRangeInvalid,
                    index);
            }
        }
        else if (commandKind == lcDyldInfoOnly)
        {
            if (!dyldInfoRangesValid(command, image.size()))
            {
                return rejected(
                    ComponentBundleMachOValidationCodeV1::LinkeditRangeInvalid,
                    index);
            }
        }
        else if (commandKind == lcCodeSignature
            || commandKind == lcSegmentSplitInfo
            || commandKind == lcFunctionStarts
            || commandKind == lcDataInCode
            || commandKind == lcDylibCodeSignDrs
            || commandKind == lcLinkerOptimizationHint
            || commandKind == lcDyldExportsTrie
            || commandKind == lcDyldChainedFixups
            || commandKind == lcAtomInfo
            || commandKind == lcFunctionVariants
            || commandKind == lcFunctionVariantFixups
            || commandKind == lcLazyLoadDylibInfo)
        {
            if (!linkeditRangeValid(command, image.size()))
            {
                return rejected(
                    ComponentBundleMachOValidationCodeV1::LinkeditRangeInvalid,
                    index);
            }
        }
        else
        {
            return rejected(
                ComponentBundleMachOValidationCodeV1::LoadCommandUnknown,
                index);
        }
        cursor += size;
    }
    if (cursor != commandEnd)
    {
        return rejected(
            ComponentBundleMachOValidationCodeV1::LoadCommandMalformed,
            commandCount);
    }
    if (!executableText)
    {
        return rejected(
            ComponentBundleMachOValidationCodeV1::ExecutableTextMissing);
    }
    return {ComponentBundleMachOValidationCodeV1::Valid, 0};
}
}
