#include "runtime/component_activation/macos/ComponentActivationSandboxReport.h"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <netinet/in.h>
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

#ifndef TALAS_COMPONENT_ACTIVATION_SANDBOX_SOURCE_BUNDLE_PATH
#error TALAS_COMPONENT_ACTIVATION_SANDBOX_SOURCE_BUNDLE_PATH is required
#endif

extern char** environ;

namespace talas::runtime::component_activation
{
namespace
{
[[nodiscard]] constexpr std::uint32_t authorityBit(
    ComponentActivationSandboxAuthorityV1 authority) noexcept
{
    return static_cast<std::uint32_t>(authority);
}

[[nodiscard]] bool deniedErrno(int value) noexcept
{
    return value == EACCES || value == EPERM;
}

void record(
    ComponentActivationSandboxReportV1& report,
    std::size_t index,
    ComponentActivationSandboxAuthorityV1 authority,
    std::int32_t result,
    bool denied) noexcept
{
    report.attemptedAuthorities |= authorityBit(authority);
    if (denied)
        report.deniedAuthorities |= authorityBit(authority);
    report.nativeResults[index] = result;
}

void probeFile(
    ComponentActivationSandboxReportV1& report,
    std::size_t index,
    ComponentActivationSandboxAuthorityV1 authority,
    const char* path) noexcept
{
    errno = 0;
    const auto descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    const auto result = descriptor >= 0 ? 0 : errno;
    if (descriptor >= 0)
        static_cast<void>(close(descriptor));
    record(report, index, authority, result, descriptor < 0 && deniedErrno(result));
}

[[nodiscard]] bool siblingHelperPath(
    std::array<char, PATH_MAX>& path,
    const char* relative) noexcept
{
    std::uint32_t bytes = static_cast<std::uint32_t>(path.size());
    if (_NSGetExecutablePath(path.data(), &bytes) != 0)
        return false;
    constexpr char marker[] = "/Contents/XPCServices/";
    auto* boundary = std::strstr(path.data(), marker);
    if (boundary == nullptr)
        return false;
    boundary += sizeof(marker) - 1U;
    const auto prefixBytes = static_cast<std::size_t>(boundary - path.data());
    const auto relativeBytes = std::strlen(relative) + 1U;
    if (prefixBytes + relativeBytes > path.size())
        return false;
    std::memcpy(boundary, relative, relativeBytes);
    return true;
}

void probeCredential(ComponentActivationSandboxReportV1& report) noexcept
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
        CFSTR("com.talas.component-activation-sandbox.credential-v1"),
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
        7,
        ComponentActivationSandboxAuthorityV1::Credentials,
        result,
        denied);
}
}

ComponentActivationSandboxReportV1
runComponentActivationSandboxReportV1(
    std::int32_t hostProcessIdentifier) noexcept
{
    ComponentActivationSandboxReportV1 report;
    report.transportProof = componentActivationSandboxSharedStorageMappedV1;
    report.hostProcessIdentifier = hostProcessIdentifier;
    probeFile(
        report,
        0,
        ComponentActivationSandboxAuthorityV1::ProjectFile,
        componentActivationSandboxProjectPathV1);

    std::array<char, PATH_MAX> path{};
    constexpr char probeRelative[] =
        "TalasComponentProbe.xpc/Contents/MacOS/TalasComponentProbe";
    if (siblingHelperPath(path, probeRelative))
    {
        probeFile(
            report,
            1,
            ComponentActivationSandboxAuthorityV1::ProbeHelper,
            path.data());
    }
    else
    {
        record(
            report,
            1,
            ComponentActivationSandboxAuthorityV1::ProbeHelper,
            EINVAL,
            false);
    }

    constexpr char realtimeRelative[] =
        "TalasComponentHost.xpc/Contents/MacOS/TalasComponentHost";
    if (siblingHelperPath(path, realtimeRelative))
    {
        probeFile(
            report,
            2,
            ComponentActivationSandboxAuthorityV1::ExistingRealtimeHelper,
            path.data());
    }
    else
    {
        record(
            report,
            2,
            ComponentActivationSandboxAuthorityV1::ExistingRealtimeHelper,
            EINVAL,
            false);
    }

    probeFile(
        report,
        3,
        ComponentActivationSandboxAuthorityV1::SourceBundle,
        TALAS_COMPONENT_ACTIVATION_SANDBOX_SOURCE_BUNDLE_PATH);

    errno = 0;
    const auto network = socket(AF_INET, SOCK_STREAM, 0);
    auto networkResult = network >= 0 ? 0 : errno;
    if (network >= 0)
    {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(componentActivationSandboxNetworkPortV1);
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
        4,
        ComponentActivationSandboxAuthorityV1::OutboundNetwork,
        networkResult,
        networkResult != 0 && deniedErrno(networkResult));

    pid_t child = 0;
    char executable[] =
        "/private/tmp/TalasComponentActivationSandbox/SpawnProbe";
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
        5,
        ComponentActivationSandboxAuthorityV1::Subprocess,
        spawnResult,
        deniedErrno(spawnResult));

    probeFile(
        report,
        6,
        ComponentActivationSandboxAuthorityV1::Device,
        componentActivationSandboxDevicePathV1);
    probeCredential(report);

    mach_port_t hostTask = MACH_PORT_NULL;
    const auto taskResult = task_for_pid(
        mach_task_self(),
        hostProcessIdentifier,
        &hostTask);
    if (taskResult == KERN_SUCCESS && hostTask != MACH_PORT_NULL)
        static_cast<void>(mach_port_deallocate(mach_task_self(), hostTask));
    record(
        report,
        8,
        ComponentActivationSandboxAuthorityV1::HostTask,
        taskResult,
        taskResult != KERN_SUCCESS);
    return report;
}
}
