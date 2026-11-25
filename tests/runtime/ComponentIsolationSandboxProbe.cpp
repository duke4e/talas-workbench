#include "runtime/macos/ComponentIsolationSandboxProbe.h"

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <mach/mach.h>
#include <netinet/in.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace talas::runtime
{
namespace
{
[[nodiscard]] constexpr std::uint32_t authorityBit(
    ComponentIsolationSandboxAuthorityV1 authority) noexcept
{
    return static_cast<std::uint32_t>(authority);
}

[[nodiscard]] bool errnoIsSandboxDenial(int value) noexcept
{
    return value == EACCES || value == EPERM;
}

void recordProbe(
    ComponentIsolationSandboxProbeReportV1& report,
    std::size_t resultIndex,
    ComponentIsolationSandboxAuthorityV1 authority,
    std::int32_t nativeResult,
    bool denied) noexcept
{
    const auto bit = authorityBit(authority);
    report.attemptedAuthorities |= bit;
    if (denied)
        report.deniedAuthorities |= bit;
    report.nativeResults[resultIndex] = nativeResult;
}

void probeFile(
    ComponentIsolationSandboxProbeReportV1& report,
    std::size_t resultIndex,
    ComponentIsolationSandboxAuthorityV1 authority,
    const char* path) noexcept
{
    errno = 0;
    const auto descriptor = open(path, O_RDONLY);
    const auto result = descriptor >= 0 ? 0 : errno;
    if (descriptor >= 0)
        static_cast<void>(close(descriptor));
    recordProbe(
        report,
        resultIndex,
        authority,
        result,
        descriptor < 0 && errnoIsSandboxDenial(result));
}
}

ComponentIsolationSandboxProbeReportV1
runComponentIsolationSandboxProbeV1(
    std::int32_t hostProcessIdentifier) noexcept
{
    ComponentIsolationSandboxProbeReportV1 report;
    report.transportProof =
        componentIsolationSandboxSharedStorageMappedV1;
    report.hostProcessIdentifier = hostProcessIdentifier;

    probeFile(
        report,
        0,
        ComponentIsolationSandboxAuthorityV1::ProjectPath,
        componentIsolationSandboxProjectPathV1);
    probeFile(
        report,
        1,
        ComponentIsolationSandboxAuthorityV1::ArbitraryFile,
        componentIsolationSandboxArbitraryPathV1);

    errno = 0;
    const auto networkSocket = socket(AF_INET, SOCK_STREAM, 0);
    int networkResult = networkSocket >= 0 ? 0 : errno;
    if (networkSocket >= 0)
    {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(componentIsolationSandboxNetworkPortV1);
        errno = 0;
        if (connect(
                networkSocket,
                reinterpret_cast<const sockaddr*>(&address),
                sizeof(address))
            != 0)
        {
            networkResult = errno;
        }
        static_cast<void>(close(networkSocket));
    }
    recordProbe(
        report,
        2,
        ComponentIsolationSandboxAuthorityV1::OutboundNetwork,
        networkResult,
        networkResult != 0 && errnoIsSandboxDenial(networkResult));

    pid_t child = 0;
    char executable[] =
        "/private/tmp/TalasComponentIsolationSandboxProbe/SpawnProbe";
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
    recordProbe(
        report,
        3,
        ComponentIsolationSandboxAuthorityV1::Subprocess,
        spawnResult,
        errnoIsSandboxDenial(spawnResult));

    probeFile(
        report,
        4,
        ComponentIsolationSandboxAuthorityV1::Device,
        componentIsolationSandboxDevicePathV1);

    mach_port_t hostTask = MACH_PORT_NULL;
    const auto taskResult = task_for_pid(
        mach_task_self(),
        hostProcessIdentifier,
        &hostTask);
    if (taskResult == KERN_SUCCESS && hostTask != MACH_PORT_NULL)
        static_cast<void>(mach_port_deallocate(mach_task_self(), hostTask));
    recordProbe(
        report,
        5,
        ComponentIsolationSandboxAuthorityV1::HostTask,
        taskResult,
        taskResult != KERN_SUCCESS);
    return report;
}
}
