#include "runtime/component_probe/macos/ComponentBundleProbeSandboxReport.h"

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <netinet/in.h>
#include <pwd.h>
#include <Security/Security.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits.h>

#ifndef TALAS_COMPONENT_PROBE_SANDBOX_SOURCE_BUNDLE_PATH
#error TALAS_COMPONENT_PROBE_SANDBOX_SOURCE_BUNDLE_PATH is required
#endif

extern char** environ;

namespace talas::runtime::component_probe
{
namespace
{
[[nodiscard]] constexpr std::uint32_t authorityBit(
    ComponentBundleProbeSandboxAuthorityV1 authority) noexcept
{
    return static_cast<std::uint32_t>(authority);
}

[[nodiscard]] bool deniedErrno(int value) noexcept
{
    return value == EACCES || value == EPERM;
}

void record(
    ComponentBundleProbeSandboxReportV1& report,
    std::size_t index,
    ComponentBundleProbeSandboxAuthorityV1 authority,
    std::int32_t result,
    bool denied) noexcept
{
    report.attemptedAuthorities |= authorityBit(authority);
    if (denied)
        report.deniedAuthorities |= authorityBit(authority);
    report.nativeResults[index] = result;
}

void probeFile(
    ComponentBundleProbeSandboxReportV1& report,
    std::size_t index,
    ComponentBundleProbeSandboxAuthorityV1 authority,
    const char* path) noexcept
{
    errno = 0;
    const auto descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    const auto result = descriptor >= 0 ? 0 : errno;
    if (descriptor >= 0)
        static_cast<void>(close(descriptor));
    record(report, index, authority, result, descriptor < 0 && deniedErrno(result));
}

void probeCredential(ComponentBundleProbeSandboxReportV1& report) noexcept
{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    const void* keys[] = {
        kSecClass,
        kSecAttrService,
        kSecAttrAccount,
        kSecMatchLimit,
        kSecReturnData,
        kSecUseAuthenticationUI,
    };
    const void* values[] = {
        kSecClassGenericPassword,
        CFSTR("com.talas.component-probe-sandbox.credential-v1"),
        CFSTR("ambient-authority"),
        kSecMatchLimitOne,
        kCFBooleanTrue,
        kSecUseAuthenticationUIFail,
    };
#pragma clang diagnostic pop
    const auto query = CFDictionaryCreate(
        kCFAllocatorDefault,
        keys,
        values,
        static_cast<CFIndex>(std::size(keys)),
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    CFTypeRef resultData = nullptr;
    const auto result = query == nullptr
        ? errSecAllocate
        : SecItemCopyMatching(query, &resultData);
    if (resultData != nullptr)
        CFRelease(resultData);
    if (query != nullptr)
        CFRelease(query);
    const auto denied = result == errSecItemNotFound
        || result == errSecAuthFailed
        || result == errSecInteractionNotAllowed
        || result == errSecMissingEntitlement;
    record(
        report,
        5,
        ComponentBundleProbeSandboxAuthorityV1::Credentials,
        result,
        denied);
}

[[nodiscard]] bool userPath(
    std::array<char, PATH_MAX>& path,
    const char* suffix) noexcept
{
    const auto* account = getpwuid(getuid());
    if (account == nullptr || account->pw_dir == nullptr)
        return false;
    const auto bytes = std::snprintf(
        path.data(),
        path.size(),
        "%s/%s",
        account->pw_dir,
        suffix);
    return bytes > 0 && static_cast<std::size_t>(bytes) < path.size();
}

[[nodiscard]] bool existingRealtimeHelperPath(
    std::array<char, PATH_MAX>& path) noexcept
{
    std::uint32_t bytes = static_cast<std::uint32_t>(path.size());
    if (_NSGetExecutablePath(path.data(), &bytes) != 0)
        return false;
    constexpr char marker[] = "/Contents/XPCServices/";
    auto* boundary = std::strstr(path.data(), marker);
    if (boundary == nullptr)
        return false;
    boundary += sizeof(marker) - 1U;
    constexpr char relative[] =
        "TalasComponentHost.xpc/Contents/MacOS/TalasComponentHost";
    const auto prefixBytes = static_cast<std::size_t>(boundary - path.data());
    if (prefixBytes + sizeof(relative) > path.size())
        return false;
    std::memcpy(boundary, relative, sizeof(relative));
    return true;
}

void probePrivateArtifact(
    ComponentBundleProbeSandboxReportV1& report) noexcept
{
    std::array<char, PATH_MAX> root{};
    const auto rootBytes = confstr(
        _CS_DARWIN_USER_TEMP_DIR,
        root.data(),
        root.size());
    if (rootBytes == 0 || rootBytes > root.size())
        return;
    std::array<char, PATH_MAX> path{};
    const auto pathBytes = std::snprintf(
        path.data(),
        path.size(),
        "%sTalasComponentProbeSandbox.XXXXXX",
        root.data());
    if (pathBytes <= 0 || static_cast<std::size_t>(pathBytes) >= path.size())
        return;
    const auto descriptor = mkstemp(path.data());
    if (descriptor < 0)
        return;
    report.privateArtifactOperations |= componentBundleProbePrivateCreateV1;
    const std::uint8_t written = 0xa5;
    if (write(descriptor, &written, sizeof(written)) == sizeof(written))
        report.privateArtifactOperations |= componentBundleProbePrivateWriteV1;
    std::uint8_t readBack = 0;
    if (pread(descriptor, &readBack, sizeof(readBack), 0) == sizeof(readBack)
        && readBack == written)
    {
        report.privateArtifactOperations |= componentBundleProbePrivateReadV1;
    }
    static_cast<void>(close(descriptor));
    if (unlink(path.data()) == 0)
        report.privateArtifactOperations |= componentBundleProbePrivateRemoveV1;
}
}

ComponentBundleProbeSandboxReportV1
runComponentBundleProbeSandboxReportV1() noexcept
{
    ComponentBundleProbeSandboxReportV1 report;
    probeFile(
        report,
        0,
        ComponentBundleProbeSandboxAuthorityV1::ProjectFile,
        componentBundleProbeSandboxProjectPathV1);

    std::array<char, PATH_MAX> path{};
    if (userPath(path, componentBundleProbeSandboxUserFileNameV1))
    {
        probeFile(
            report,
            1,
            ComponentBundleProbeSandboxAuthorityV1::UserFile,
            path.data());
    }
    else
    {
        record(
            report,
            1,
            ComponentBundleProbeSandboxAuthorityV1::UserFile,
            EINVAL,
            false);
    }

    errno = 0;
    const auto network = socket(AF_INET, SOCK_STREAM, 0);
    auto networkResult = network >= 0 ? 0 : errno;
    if (network >= 0)
    {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(componentBundleProbeSandboxNetworkPortV1);
        errno = 0;
        if (connect(
                network,
                reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) != 0)
        {
            networkResult = errno;
        }
        static_cast<void>(close(network));
    }
    record(
        report,
        2,
        ComponentBundleProbeSandboxAuthorityV1::OutboundNetwork,
        networkResult,
        networkResult != 0 && deniedErrno(networkResult));

    pid_t child = 0;
    char executable[] =
        "/private/tmp/TalasComponentProbeSandbox/SpawnProbe";
    char* arguments[] = {executable, nullptr};
    const auto spawnResult = posix_spawn(
        &child,
        executable,
        nullptr,
        nullptr,
        arguments,
        environ);
    if (spawnResult == 0)
    {
        int status = 0;
        static_cast<void>(waitpid(child, &status, 0));
    }
    record(
        report,
        3,
        ComponentBundleProbeSandboxAuthorityV1::Subprocess,
        spawnResult,
        deniedErrno(spawnResult));

    probeFile(
        report,
        4,
        ComponentBundleProbeSandboxAuthorityV1::Device,
        componentBundleProbeSandboxDevicePathV1);

    probeCredential(report);

    probeFile(
        report,
        6,
        ComponentBundleProbeSandboxAuthorityV1::RealtimeSharedStorage,
        componentBundleProbeSandboxRealtimePathV1);

    if (existingRealtimeHelperPath(path))
    {
        probeFile(
            report,
            7,
            ComponentBundleProbeSandboxAuthorityV1::ExistingRealtimeHelper,
            path.data());
    }
    else
    {
        record(
            report,
            7,
            ComponentBundleProbeSandboxAuthorityV1::ExistingRealtimeHelper,
            EINVAL,
            false);
    }

    probeFile(
        report,
        8,
        ComponentBundleProbeSandboxAuthorityV1::SourceBundle,
        TALAS_COMPONENT_PROBE_SANDBOX_SOURCE_BUNDLE_PATH);
    probePrivateArtifact(report);
    return report;
}
}
