#include "runtime/component_probe/macos/ComponentBundleProbeMaterialization.h"

#include <CommonCrypto/CommonDigest.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{
namespace probe = talas::runtime::component_probe;
using namespace talas::runtime;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

probe::ComponentBundleProbeRequestV1 request()
{
    probe::ComponentBundleProbeRequestV1 value;
    value.header = probe::componentBundleProbeHeaderV1(
        probe::ComponentBundleProbeRecordKindV1::Request,
        sizeof(value));
    constexpr std::string_view bundleId = "org.talas.materialization";
    constexpr std::string_view version = "1.0.0";
    value.bundleIdBytes = bundleId.size();
    value.semanticVersionBytes = version.size();
    std::copy(bundleId.begin(), bundleId.end(), value.bundleId.begin());
    std::copy(version.begin(), version.end(), value.semanticVersion.begin());
    value.contentDigest.fill(0x31);
    value.signerIdentityDigest.fill(0x73);
    value.requestedPermissions =
        componentPermission(ComponentExecutionPermission::RealtimeAudio);
    value.declaredRoles =
        componentRole(ComponentExecutionRole::RealtimeProcessor);
    value.policyVersion = currentComponentTrustPolicyVersion;
    value.entryByteCount = 4;
    value.artifactGeneration = static_cast<std::uint64_t>(getpid());
    value.validationNonce.fill(0x5c);
    value.validationNonce.front() = static_cast<std::uint8_t>(getpid());
    return value;
}

probe::ComponentBundleProbeExpectedAbiV1 expected(
    const probe::ComponentBundleProbeRequestV1& requestValue,
    const std::array<std::uint8_t, 4>& bytes)
{
    probe::ComponentBundleProbeExpectedAbiV1 value;
    value.header = probe::componentBundleProbeHeaderV1(
        probe::ComponentBundleProbeRecordKindV1::ExpectedAbi,
        sizeof(value));
    require(
        CC_SHA256(
            bytes.data(),
            static_cast<CC_LONG>(bytes.size()),
            value.entryDigest.data()) != nullptr,
        "materialization test digest should compute");
    value.entryByteCount = requestValue.entryByteCount;
    value.minimumEngineAbiVersion = TALAS_KERNEL_ABI_VERSION_V1;
    value.maximumEngineAbiVersion = TALAS_KERNEL_ABI_VERSION_V1;
    value.minimumEngineApiVersion = TALAS_KERNEL_API_VERSION_V1;
    value.maximumEngineApiVersion = TALAS_KERNEL_API_VERSION_V1;
    constexpr std::string_view nodeId =
        "org.talas.materialization.oscillator";
    value.nodeCount = 1;
    value.nodeIdBytes[0] = nodeId.size();
    std::copy(nodeId.begin(), nodeId.end(), value.nodeIds[0].begin());
    return value;
}

void testExactSessionPathAndCleanup()
{
    const auto requestValue = request();
    const std::array<std::uint8_t, 4> bytes{1, 2, 3, 4};
    const auto expectedValue = expected(requestValue, bytes);
    std::array<char, PATH_MAX> path{};
    require(
        probe::componentBundleProbeMaterializationPathV1(requestValue, path),
        "valid request should derive a bounded private path");
    require(mkdir(path.data(), 0700) == 0, "private test directory should create");
    const auto directory = open(
        path.data(),
        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    require(directory >= 0, "private test directory should open exactly");
    const auto file = openat(
        directory,
        "entry.bundle",
        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
        0600);
    require(file >= 0, "private test entry should create exactly");
    require(
        write(file, bytes.data(), bytes.size())
                == static_cast<ssize_t>(bytes.size())
            && fchmod(file, 0500) == 0 && close(file) == 0
            && close(directory) == 0,
        "private test entry should seal exactly");
    require(
        probe::cleanupComponentBundleProbeMaterializationV1(
            requestValue,
            expectedValue,
            std::as_bytes(std::span{bytes})),
        "exact session materialization should clean");
    struct stat status{};
    require(
        lstat(path.data(), &status) != 0 && errno == ENOENT,
        "exact session directory should be gone");
}

void testSuspiciousPathIdentityIsNeverRemoved()
{
    auto requestValue = request();
    requestValue.validationNonce.back() ^= 0x1U;
    const std::array<std::uint8_t, 4> bytes{1, 2, 3, 4};
    const auto expectedValue = expected(requestValue, bytes);
    std::array<char, PATH_MAX> path{};
    require(
        probe::componentBundleProbeMaterializationPathV1(requestValue, path)
            && mkdir(path.data(), 0755) == 0,
        "suspicious test directory should create");
    require(
        !probe::cleanupComponentBundleProbeMaterializationV1(
            requestValue,
            expectedValue,
            std::as_bytes(std::span{bytes})),
        "unexpected directory permissions should fail closed");
    require(rmdir(path.data()) == 0, "test-owned suspicious directory should remove");
}

void testInterruptedPrefixIsCleanedExactly()
{
    auto requestValue = request();
    requestValue.validationNonce.back() ^= 0x2U;
    const std::array<std::uint8_t, 4> bytes{1, 2, 3, 4};
    const auto expectedValue = expected(requestValue, bytes);
    std::array<char, PATH_MAX> path{};
    require(
        probe::componentBundleProbeMaterializationPathV1(requestValue, path)
            && mkdir(path.data(), 0700) == 0,
        "interrupted private directory should create");
    const auto directory = open(
        path.data(),
        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    require(directory >= 0, "interrupted private directory should open");
    const auto file = openat(
        directory,
        "entry.bundle",
        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
        0600);
    require(file >= 0, "interrupted entry should create");
    require(
        write(file, bytes.data(), 2) == 2 && close(file) == 0
            && close(directory) == 0,
        "interrupted entry prefix should write");
    require(
        probe::cleanupComponentBundleProbeMaterializationV1(
            requestValue,
            expectedValue,
            std::as_bytes(std::span{bytes})),
        "exact interrupted prefix should clean");
    struct stat status{};
    require(
        lstat(path.data(), &status) != 0 && errno == ENOENT,
        "interrupted private directory should be gone");
}
}

std::filesystem::path makeStaleCandidate(std::string_view name, bool extraFile, std::time_t age)
{
    std::array<char, PATH_MAX> root{};
    const auto bytes = confstr(_CS_DARWIN_USER_TEMP_DIR, root.data(), root.size());
    require(bytes != 0 && bytes <= root.size(), "temporary root should resolve");
    const auto directory = std::filesystem::path{root.data()} / name;
    std::filesystem::remove_all(directory);
    require(::mkdir(directory.c_str(), 0700) == 0, "sweep candidate should be created");
    std::ofstream(directory / "entry.bundle", std::ios::binary) << "image";
    require(::chmod((directory / "entry.bundle").c_str(), 0500) == 0, "sweep candidate entry mode");
    if (extraFile)
        std::ofstream(directory / "notes.txt") << "not a probe materialization";
    const auto then = std::time(nullptr) - age;
    const std::array<timeval, 2> times{timeval{then, 0}, timeval{then, 0}};
    require(::utimes(directory.c_str(), times.data()) == 0, "sweep candidate age should be set");
    return directory;
}

void testHelperStartupSweepsOnlyStaleMaterializations()
{
    const auto suffix = std::to_string(getpid());
    const auto stale = makeStaleCandidate("TalasComponentProbe.v1.sweep-stale-" + suffix, false, 3600);
    const auto fresh = makeStaleCandidate("TalasComponentProbe.v1.sweep-fresh-" + suffix, false, 0);
    const auto foreign = makeStaleCandidate("TalasComponentProbe.v1.sweep-foreign-" + suffix, true, 3600);
    const auto removed = probe::sweepStaleComponentBundleProbeMaterializationsV1(600);
    const auto staleGone = !std::filesystem::exists(stale);
    const auto freshKept = std::filesystem::exists(fresh / "entry.bundle");
    const auto foreignKept = std::filesystem::exists(foreign / "notes.txt");
    std::filesystem::remove_all(stale);
    std::filesystem::remove_all(fresh);
    std::filesystem::remove_all(foreign);
    require(removed >= 1 && staleGone, "a stale materialization should be swept");
    require(freshKept, "a materialization that may still be in use must be kept");
    require(foreignKept, "a directory with unexpected contents must be kept");
}

int main()
{
    testExactSessionPathAndCleanup();
    testSuspiciousPathIdentityIsNeverRemoved();
    testInterruptedPrefixIsCleanedExactly();
    testHelperStartupSweepsOnlyStaleMaterializations();
    return 0;
}
