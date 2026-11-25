#include "runtime/component_probe/ComponentBundleMachOValidation.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// These synthetic Mach-O records are original Talas Apache-2.0 test
// fixtures. They are not derived from the local-only reference corpus.

namespace
{
namespace probe = talas::runtime::component_probe;

constexpr std::uint32_t lcSegment64 = 0x19;
constexpr std::uint32_t lcLoadDylib = 0x0c;
constexpr std::uint32_t lcIdDylib = 0x0d;
constexpr std::uint32_t lcLoadDylinker = 0x0e;
constexpr std::uint32_t lcRpath = 0x8000001c;
constexpr std::uint32_t lcFunctionStarts = 0x26;
constexpr std::uint32_t lcBuildVersion = 0x32;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void appendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
    for (std::size_t index = 0; index < 4; ++index)
    {
        bytes.push_back(static_cast<std::uint8_t>(
            value >> (index * 8U)));
    }
}

void appendU64(std::vector<std::uint8_t>& bytes, std::uint64_t value)
{
    for (std::size_t index = 0; index < 8; ++index)
    {
        bytes.push_back(static_cast<std::uint8_t>(
            value >> (index * 8U)));
    }
}

void writeU32(
    std::vector<std::uint8_t>& bytes,
    std::size_t offset,
    std::uint32_t value)
{
    for (std::size_t index = 0; index < 4; ++index)
    {
        bytes[offset + index] = static_cast<std::uint8_t>(
            value >> (index * 8U));
    }
}

std::uint32_t readU32(
    const std::vector<std::uint8_t>& bytes,
    std::size_t offset)
{
    return static_cast<std::uint32_t>(bytes[offset])
        | (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U)
        | (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U)
        | (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

std::uint64_t readU64(
    const std::vector<std::uint8_t>& bytes,
    std::size_t offset)
{
    return static_cast<std::uint64_t>(readU32(bytes, offset))
        | (static_cast<std::uint64_t>(readU32(bytes, offset + 4U)) << 32U);
}

void writeU64(
    std::vector<std::uint8_t>& bytes,
    std::size_t offset,
    std::uint64_t value)
{
    for (std::size_t index = 0; index < 8; ++index)
    {
        bytes[offset + index] = static_cast<std::uint8_t>(
            value >> (index * 8U));
    }
}

std::vector<std::uint8_t> segment(
    std::string_view name,
    std::uint64_t vmAddress,
    std::uint64_t vmBytes,
    std::uint64_t fileOffset,
    std::uint64_t fileBytes,
    std::uint32_t maximumProtection,
    std::uint32_t initialProtection)
{
    std::vector<std::uint8_t> command;
    appendU32(command, lcSegment64);
    appendU32(command, 72);
    command.resize(24);
    std::copy(name.begin(), name.end(), command.begin() + 8);
    appendU64(command, vmAddress);
    appendU64(command, vmBytes);
    appendU64(command, fileOffset);
    appendU64(command, fileBytes);
    appendU32(command, maximumProtection);
    appendU32(command, initialProtection);
    appendU32(command, 0);
    appendU32(command, 0);
    return command;
}

std::vector<std::uint8_t> textSegmentWithSections()
{
    auto command = segment(
        "__TEXT",
        0x100000000,
        0x1000,
        0,
        0,
        5,
        5);
    writeU32(command, 4, 72U + 2U * 80U);
    writeU32(command, 64, 2);
    command.resize(72U + 2U * 80U);
    for (std::size_t index = 0; index < 2; ++index)
    {
        const auto offset = 72U + index * 80U;
        const auto name = index == 0
            ? std::string_view{"__text"}
            : std::string_view{"__const"};
        std::copy(name.begin(), name.end(), command.data() + offset);
        constexpr std::string_view segmentName = "__TEXT";
        std::copy(
            segmentName.begin(),
            segmentName.end(),
            command.data() + offset + 16U);
        writeU64(
            command,
            offset + 32U,
            0x100000100U + index * 8U);
        writeU64(command, offset + 40U, 8);
        writeU32(command, offset + 52U, 3);
    }
    return command;
}

std::vector<std::uint8_t> pathCommand(
    std::uint32_t kind,
    std::string_view path)
{
    constexpr std::size_t pathOffset = 24;
    auto commandBytes = pathOffset + path.size() + 1U;
    commandBytes = (commandBytes + 7U) & ~std::size_t{7U};
    std::vector<std::uint8_t> command;
    appendU32(command, kind);
    appendU32(command, static_cast<std::uint32_t>(commandBytes));
    appendU32(command, pathOffset);
    appendU32(command, 0);
    appendU32(command, 0);
    appendU32(command, 0);
    command.resize(commandBytes);
    std::copy(path.begin(), path.end(), command.begin() + pathOffset);
    return command;
}

std::vector<std::uint8_t> buildVersion()
{
    std::vector<std::uint8_t> command;
    appendU32(command, lcBuildVersion);
    appendU32(command, 24);
    appendU32(command, 1);
    appendU32(command, 0x000d0000);
    appendU32(command, 0x000d0000);
    appendU32(command, 0);
    return command;
}

std::vector<std::uint8_t> rawCommand(std::uint32_t kind)
{
    std::vector<std::uint8_t> command;
    appendU32(command, kind);
    appendU32(command, 8);
    return command;
}

std::vector<std::uint8_t> linkeditCommand(
    std::uint32_t kind,
    std::uint32_t offset,
    std::uint32_t size)
{
    std::vector<std::uint8_t> command;
    appendU32(command, kind);
    appendU32(command, 16);
    appendU32(command, offset);
    appendU32(command, size);
    return command;
}

std::vector<std::uint8_t> image(
    std::vector<std::vector<std::uint8_t>> commands,
    std::uint32_t cpuType = 0x0100000c,
    std::uint32_t cpuSubtype = 0,
    std::uint32_t fileType = 8)
{
    std::size_t commandBytes = 0;
    for (const auto& command : commands)
        commandBytes += command.size();

    std::vector<std::uint8_t> bytes;
    appendU32(bytes, 0xfeedfacf);
    appendU32(bytes, cpuType);
    appendU32(bytes, cpuSubtype);
    appendU32(bytes, fileType);
    appendU32(bytes, static_cast<std::uint32_t>(commands.size()));
    appendU32(bytes, static_cast<std::uint32_t>(commandBytes));
    appendU32(bytes, 0x85);
    appendU32(bytes, 0);
    for (const auto& command : commands)
        bytes.insert(bytes.end(), command.begin(), command.end());
    bytes.resize(bytes.size() + 64U, 0xa5);
    return bytes;
}

std::vector<std::uint8_t> validImage()
{
    auto bytes = image({
        textSegmentWithSections(),
        pathCommand(lcLoadDylib, "/usr/lib/libSystem.B.dylib"),
        buildVersion(),
    });
    writeU64(bytes, 32U + 48U, bytes.size());
    const auto sectionBytes = 32U + readU32(bytes, 20);
    writeU32(bytes, 32U + 72U + 48U, sectionBytes);
    writeU32(bytes, 32U + 72U + 80U + 48U, sectionBytes + 8U);
    return bytes;
}

void requireCode(
    const std::vector<std::uint8_t>& bytes,
    probe::ComponentBundleMachOValidationCodeV1 code,
    std::string_view message)
{
    const auto result = probe::validateComponentBundleMachOV1(
        bytes,
        probe::ComponentBundleMachOArchitectureV1::Arm64);
    require(result.code == code, std::string(message));
}

void testCleanImageAndClosedHeader()
{
    requireCode(
        validImage(),
        probe::ComponentBundleMachOValidationCodeV1::Valid,
        "one thin arm64 bundle with a system dependency should validate");

    auto truncated = validImage();
    truncated.resize(12);
    requireCode(
        truncated,
        probe::ComponentBundleMachOValidationCodeV1::HeaderTruncated,
        "a truncated Mach-O header must reject");

    auto fat = validImage();
    writeU32(fat, 0, 0xcafebabe);
    requireCode(
        fat,
        probe::ComponentBundleMachOValidationCodeV1::FatContainerRejected,
        "fat architecture ambiguity must reject");

    auto wrongArchitecture = validImage();
    writeU32(wrongArchitecture, 4, 0x01000007);
    requireCode(
        wrongArchitecture,
        probe::ComponentBundleMachOValidationCodeV1::ArchitectureMismatch,
        "a different architecture must reject");

    auto wrongSubtype = validImage();
    writeU32(wrongSubtype, 8, 2);
    requireCode(
        wrongSubtype,
        probe::ComponentBundleMachOValidationCodeV1::CpuSubtypeRejected,
        "arm64e is not admitted by the v1 native arm64 profile");

    auto executable = validImage();
    writeU32(executable, 12, 2);
    requireCode(
        executable,
        probe::ComponentBundleMachOValidationCodeV1::FileTypeRejected,
        "only MH_BUNDLE is accepted");

    auto unsafeFlags = validImage();
    writeU32(unsafeFlags, 24, 0x85U | 0x20000U);
    requireCode(
        unsafeFlags,
        probe::ComponentBundleMachOValidationCodeV1::HeaderFlagsRejected,
        "executable-stack or unknown header flags must reject");

    requireCode(
        std::vector<std::uint8_t>{},
        probe::ComponentBundleMachOValidationCodeV1::EmptyImage,
        "an empty image must reject");

    requireCode(
        std::vector<std::uint8_t>(
            probe::maximumComponentBundleProbeEntryBytesV1 + 1U),
        probe::ComponentBundleMachOValidationCodeV1::EntryBudgetExceeded,
        "an image above the 16 MiB probe ceiling must reject");
}

void testOriginalCompilerProducedFixture()
{
#if defined(TALAS_COMPONENT_PROBE_CLEAN_FIXTURE_PATH) \
    && !defined(TALAS_COMPONENT_PROBE_SANITIZED_BUILD)
    std::ifstream input(
        TALAS_COMPONENT_PROBE_CLEAN_FIXTURE_PATH,
        std::ios::binary);
    require(
        static_cast<bool>(input),
        "the original test-only probe module should be readable");
    const std::vector<std::uint8_t> bytes{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}};
    const auto architecture =
        probe::currentComponentBundleMachOArchitectureV1();
    require(
        architecture
            != probe::ComponentBundleMachOArchitectureV1::Unsupported,
        "the macOS test architecture should be closed and supported");
    const auto result = probe::validateComponentBundleMachOV1(
        bytes,
        architecture);
    require(
        result.ok(),
        "the original compiler-produced MH_BUNDLE should validate; code="
            + std::to_string(static_cast<unsigned>(result.code))
            + " command=" + std::to_string(result.commandIndex));

    auto wrongArchitecture = bytes;
    writeU32(wrongArchitecture, 4, 0);
    require(
        probe::validateComponentBundleMachOV1(
            wrongArchitecture,
            architecture)
                .code
            == probe::ComponentBundleMachOValidationCodeV1::
                ArchitectureMismatch,
        "an adversarial architecture mutation of the original fixture must reject");

    auto writableExecutable = bytes;
    auto cursor = std::size_t{32};
    bool mutatedText = false;
    for (std::uint32_t index = 0;
         index < readU32(writableExecutable, 16);
         ++index)
    {
        const auto command = readU32(writableExecutable, cursor);
        const auto commandBytes = readU32(writableExecutable, cursor + 4U);
        if (command == lcSegment64
            && std::string_view{
                reinterpret_cast<const char*>(
                    writableExecutable.data() + cursor + 8U)}
                == "__TEXT")
        {
            writeU32(writableExecutable, cursor + 56U, 7);
            writeU32(writableExecutable, cursor + 60U, 7);
            mutatedText = true;
            break;
        }
        cursor += commandBytes;
    }
    require(mutatedText, "the original fixture should expose one __TEXT segment");
    require(
        probe::validateComponentBundleMachOV1(
            writableExecutable,
            architecture)
                .code
            == probe::ComponentBundleMachOValidationCodeV1::
                WritableExecutableSegment,
        "an adversarial W+X mutation of the original fixture must reject");

    auto nonSystemDependency = bytes;
    constexpr std::string_view systemPrefix = "/usr/lib/";
    constexpr std::string_view privatePrefix = "/tmp/lib/";
    const auto dependency = std::search(
        nonSystemDependency.begin(),
        nonSystemDependency.end(),
        systemPrefix.begin(),
        systemPrefix.end());
    require(
        dependency != nonSystemDependency.end(),
        "the original fixture should contain one canonical system dependency");
    std::copy(
        privatePrefix.begin(),
        privatePrefix.end(),
        dependency);
    require(
        probe::validateComponentBundleMachOV1(
            nonSystemDependency,
            architecture)
                .code
            == probe::ComponentBundleMachOValidationCodeV1::
                NonSystemDependency,
        "an adversarial dependency mutation of the original fixture must reject");

    auto overlappingSections = bytes;
    cursor = 32;
    bool mutatedSections = false;
    for (std::uint32_t index = 0;
         index < readU32(overlappingSections, 16) && !mutatedSections;
         ++index)
    {
        const auto command = readU32(overlappingSections, cursor);
        const auto commandBytes = readU32(overlappingSections, cursor + 4U);
        if (command == lcSegment64)
        {
            const auto sectionCount = readU32(overlappingSections, cursor + 64U);
            std::size_t first = 0;
            for (std::size_t section = 0;
                 section < sectionCount;
                 ++section)
            {
                const auto record = cursor + 72U + section * 80U;
                if (readU64(overlappingSections, record + 40U) == 0)
                    continue;
                if (first == 0)
                {
                    first = record;
                    continue;
                }
                writeU64(
                    overlappingSections,
                    record + 32U,
                    readU64(overlappingSections, first + 32U));
                writeU64(
                    overlappingSections,
                    record + 40U,
                    readU64(overlappingSections, first + 40U));
                writeU32(
                    overlappingSections,
                    record + 48U,
                    readU32(overlappingSections, first + 48U));
                mutatedSections = true;
                break;
            }
        }
        cursor += commandBytes;
    }
    require(
        mutatedSections,
        "the original fixture should expose two file-backed sections");
    require(
        probe::validateComponentBundleMachOV1(
            overlappingSections,
            architecture)
                .code
            == probe::ComponentBundleMachOValidationCodeV1::
                SectionMalformed,
        "an adversarial section-overlap mutation must reject before loading");
#endif
}

void testCommandAndSegmentBounds()
{
    auto commandOverflow = validImage();
    writeU32(commandOverflow, 20, 0xffffffffU);
    requireCode(
        commandOverflow,
        probe::ComponentBundleMachOValidationCodeV1::
            LoadCommandBudgetExceeded,
        "load-command bytes above the fixed budget must reject");

    auto commandTruncated = validImage();
    writeU32(commandTruncated, 20, 4096);
    requireCode(
        commandTruncated,
        probe::ComponentBundleMachOValidationCodeV1::LoadCommandsTruncated,
        "a budget-valid command region beyond the image must reject");

    auto malformedCommand = validImage();
    writeU32(malformedCommand, 36, 7);
    requireCode(
        malformedCommand,
        probe::ComponentBundleMachOValidationCodeV1::LoadCommandMalformed,
        "undersized or unaligned commands must reject");

    auto unknownCommand = validImage();
    writeU32(unknownCommand, 32, 0x7ffffffe);
    requireCode(
        unknownCommand,
        probe::ComponentBundleMachOValidationCodeV1::LoadCommandUnknown,
        "unknown load commands must fail closed");

    auto commandCountBudget = validImage();
    writeU32(
        commandCountBudget,
        16,
        probe::maximumComponentBundleMachOLoadCommandsV1 + 1U);
    requireCode(
        commandCountBudget,
        probe::ComponentBundleMachOValidationCodeV1::
            LoadCommandBudgetExceeded,
        "the load-command count must remain fixed-budget");

    auto writableExecutable = image({
        segment("__TEXT", 0x100000000, 0x1000, 0, 0, 7, 7),
        buildVersion(),
    });
    writeU64(writableExecutable, 32U + 48U, writableExecutable.size());
    requireCode(
        writableExecutable,
        probe::ComponentBundleMachOValidationCodeV1::WritableExecutableSegment,
        "W+X segments must reject");

    auto overlapping = image({
        segment("__TEXT", 0x100000000, 0x1000, 0, 0, 5, 5),
        segment("__DATA", 0x100002000, 0x1000, 16, 32, 3, 3),
        buildVersion(),
    });
    writeU64(overlapping, 32U + 48U, overlapping.size());
    requireCode(
        overlapping,
        probe::ComponentBundleMachOValidationCodeV1::SegmentFileRangeOverlap,
        "overlapping file-backed segments must reject");

    auto vmOverlapping = image({
        segment("__TEXT", 0x100000000, 0x1000, 0, 0, 5, 5),
        segment("__DATA", 0x100000800, 0x1000, 0, 0, 3, 3),
        buildVersion(),
    });
    requireCode(
        vmOverlapping,
        probe::ComponentBundleMachOValidationCodeV1::SegmentVmRangeOverlap,
        "overlapping VM segments must reject");

    auto badFileRange = validImage();
    writeU64(badFileRange, 32U + 40U, badFileRange.size() + 1U);
    requireCode(
        badFileRange,
        probe::ComponentBundleMachOValidationCodeV1::SegmentFileRangeInvalid,
        "segment file ranges outside the frozen image must reject");

    std::vector<std::vector<std::uint8_t>> tooManySegments;
    for (std::size_t index = 0;
         index <= probe::maximumComponentBundleMachOSegmentsV1;
         ++index)
    {
        tooManySegments.push_back(
            segment("__S", 0, 0, 0, 0, 0, 0));
    }
    requireCode(
        image(std::move(tooManySegments)),
        probe::ComponentBundleMachOValidationCodeV1::SegmentBudgetExceeded,
        "the segment table must remain fixed-budget");

    auto badLinkedit = image({
        segment("__TEXT", 0x100000000, 0x1000, 0, 0, 5, 5),
        linkeditCommand(lcFunctionStarts, 0xffffffffU, 16),
        buildVersion(),
    });
    writeU64(badLinkedit, 32U + 48U, badLinkedit.size());
    requireCode(
        badLinkedit,
        probe::ComponentBundleMachOValidationCodeV1::LinkeditRangeInvalid,
        "linkedit payloads outside the frozen image must reject");

    auto wrongPlatformCommand = buildVersion();
    writeU32(wrongPlatformCommand, 8, 2);
    auto wrongPlatform = image({
        segment("__TEXT", 0x100000000, 0x1000, 0, 0, 5, 5),
        std::move(wrongPlatformCommand),
    });
    writeU64(wrongPlatform, 32U + 48U, wrongPlatform.size());
    requireCode(
        wrongPlatform,
        probe::ComponentBundleMachOValidationCodeV1::PlatformRejected,
        "a non-macOS build-version command must reject");
}

void testDependencyPolicy()
{
    auto rpath = image({
        segment("__TEXT", 0x100000000, 0x1000, 0, 0, 5, 5),
        pathCommand(lcRpath, "@loader_path/Frameworks"),
        buildVersion(),
    });
    writeU64(rpath, 32U + 48U, rpath.size());
    requireCode(
        rpath,
        probe::ComponentBundleMachOValidationCodeV1::LoaderPathRejected,
        "LC_RPATH must reject");

    auto indirect = image({
        segment("__TEXT", 0x100000000, 0x1000, 0, 0, 5, 5),
        pathCommand(lcLoadDylib, "@rpath/libEscape.dylib"),
        buildVersion(),
    });
    writeU64(indirect, 32U + 48U, indirect.size());
    requireCode(
        indirect,
        probe::ComponentBundleMachOValidationCodeV1::LoaderPathRejected,
        "loader path indirection must reject");

    auto nonSystem = image({
        segment("__TEXT", 0x100000000, 0x1000, 0, 0, 5, 5),
        pathCommand(lcLoadDylib, "/tmp/libEscape.dylib"),
        buildVersion(),
    });
    writeU64(nonSystem, 32U + 48U, nonSystem.size());
    requireCode(
        nonSystem,
        probe::ComponentBundleMachOValidationCodeV1::NonSystemDependency,
        "non-system dependencies must reject");

    auto nonCanonicalSystem = image({
        segment("__TEXT", 0x100000000, 0x1000, 0, 0, 5, 5),
        pathCommand(lcLoadDylib, "/usr/lib/../tmp/libEscape.dylib"),
        buildVersion(),
    });
    writeU64(
        nonCanonicalSystem,
        32U + 48U,
        nonCanonicalSystem.size());
    requireCode(
        nonCanonicalSystem,
        probe::ComponentBundleMachOValidationCodeV1::NonSystemDependency,
        "system-prefix dependencies with traversal must reject");

    auto installName = image({
        segment("__TEXT", 0x100000000, 0x1000, 0, 0, 5, 5),
        pathCommand(lcIdDylib, "/tmp/claimed.dylib"),
        buildVersion(),
    });
    writeU64(installName, 32U + 48U, installName.size());
    requireCode(
        installName,
        probe::ComponentBundleMachOValidationCodeV1::InstallNameRejected,
        "bundle images cannot claim an install name");

    auto dylinker = image({
        segment("__TEXT", 0x100000000, 0x1000, 0, 0, 5, 5),
        rawCommand(lcLoadDylinker),
        buildVersion(),
    });
    writeU64(dylinker, 32U + 48U, dylinker.size());
    requireCode(
        dylinker,
        probe::ComponentBundleMachOValidationCodeV1::DylinkerRejected,
        "a bundle cannot select a dynamic linker or process entry point");

    std::vector<std::vector<std::uint8_t>> tooManyDependencies;
    tooManyDependencies.push_back(
        segment("__TEXT", 0x100000000, 0x1000, 0, 0, 5, 5));
    for (std::size_t index = 0;
         index <= probe::maximumComponentBundleMachODependenciesV1;
         ++index)
    {
        tooManyDependencies.push_back(
            pathCommand(lcLoadDylib, "/usr/lib/libSystem.B.dylib"));
    }
    auto dependencyBudget = image(std::move(tooManyDependencies));
    writeU64(dependencyBudget, 32U + 48U, dependencyBudget.size());
    requireCode(
        dependencyBudget,
        probe::ComponentBundleMachOValidationCodeV1::
            DependencyBudgetExceeded,
        "the dependency table must remain fixed-budget");
}

void testMutationAndTruncationSweepIsTotal()
{
    const auto clean = validImage();
    for (std::size_t size = 0; size < clean.size(); ++size)
    {
        static_cast<void>(probe::validateComponentBundleMachOV1(
            std::span<const std::uint8_t>{clean}.first(size),
            probe::ComponentBundleMachOArchitectureV1::Arm64));
    }
    for (std::size_t offset = 0; offset < clean.size(); ++offset)
    {
        auto mutated = clean;
        mutated[offset] ^= static_cast<std::uint8_t>(
            0x5aU + (offset & 0x1fU));
        static_cast<void>(probe::validateComponentBundleMachOV1(
            mutated,
            probe::ComponentBundleMachOArchitectureV1::Arm64));
    }
    static_assert(noexcept(probe::validateComponentBundleMachOV1(
        std::span<const std::uint8_t>{},
        probe::ComponentBundleMachOArchitectureV1::Arm64)));
}
}

int main()
{
    testCleanImageAndClosedHeader();
    testOriginalCompilerProducedFixture();
    testCommandAndSegmentBounds();
    testDependencyPolicy();
    testMutationAndTruncationSweepIsTotal();
    return 0;
}
