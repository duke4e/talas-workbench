#include "talas/runtime/KernelAbiProbe.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#elif defined(__APPLE__)
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace talas::runtime
{
namespace
{
[[nodiscard]] std::string_view argumentFor(
    KernelAbiProbeCase probeCase) noexcept
{
    switch (probeCase)
    {
    case KernelAbiProbeCase::CleanExit:
        return "clean";
    case KernelAbiProbeCase::MetadataRejected:
        return "metadata-rejected";
    case KernelAbiProbeCase::ControlledFatal:
        return "controlled-fatal";
    case KernelAbiProbeCase::AbnormalTermination:
        return "abnormal-termination";
    case KernelAbiProbeCase::AbnormalTerminationNormalExit:
        return "abnormal-termination-normal-exit";
    case KernelAbiProbeCase::BoundedTimeout:
        return "bounded-timeout";
    }
    return "invalid";
}

[[nodiscard]] KernelAbiProbeCase classificationCaseFor(
    KernelAbiProbeCase probeCase) noexcept
{
    return probeCase
            == KernelAbiProbeCase::AbnormalTerminationNormalExit
        ? KernelAbiProbeCase::AbnormalTermination
        : probeCase;
}

void appendDiagnostic(
    KernelAbiProbeResult& result,
    const char* bytes,
    std::size_t byteCount)
{
    const auto available = maximumKernelAbiProbeDiagnosticBytes
        - result.diagnostics.size();
    const auto copied = std::min(available, byteCount);
    result.diagnostics.append(bytes, copied);
    if (copied != byteCount)
        result.diagnosticsTruncated = true;
}

void classifyNormalExit(
    KernelAbiProbeResult& result,
    KernelAbiProbeCase probeCase,
    int exitCode) noexcept
{
    result.exitCode = exitCode;
    const auto expected = probeCase == KernelAbiProbeCase::CleanExit
        ? 0
        : probeCase == KernelAbiProbeCase::MetadataRejected
        ? 20
        : probeCase == KernelAbiProbeCase::ControlledFatal
        ? 21
        : -1;
    if (exitCode != expected)
    {
        result.outcome = KernelAbiProbeOutcome::ProtocolError;
        return;
    }
    switch (probeCase)
    {
    case KernelAbiProbeCase::CleanExit:
        result.outcome = KernelAbiProbeOutcome::CleanExit;
        break;
    case KernelAbiProbeCase::MetadataRejected:
        result.outcome = KernelAbiProbeOutcome::MetadataRejected;
        break;
    case KernelAbiProbeCase::ControlledFatal:
        result.outcome = KernelAbiProbeOutcome::ControlledFatal;
        break;
    case KernelAbiProbeCase::AbnormalTermination:
        result.outcome = KernelAbiProbeOutcome::ProtocolError;
        break;
    case KernelAbiProbeCase::AbnormalTerminationNormalExit:
        result.outcome = KernelAbiProbeOutcome::ProtocolError;
        break;
    case KernelAbiProbeCase::BoundedTimeout:
        result.outcome = KernelAbiProbeOutcome::ProtocolError;
        break;
    }
}

#if defined(_WIN32)
[[nodiscard]] bool isWindowsCrashStatus(DWORD exitCode) noexcept
{
    return exitCode == 3
        || (exitCode & 0xf0000000U) == 0xc0000000U;
}

[[nodiscard]] std::filesystem::path probeExecutablePath()
{
    std::vector<wchar_t> path(32'768);
    const auto size = GetModuleFileNameW(
        nullptr,
        path.data(),
        static_cast<DWORD>(path.size()));
    if (size == 0 || size >= path.size())
        return {};
    return std::filesystem::path(path.data(), path.data() + size)
        .parent_path()
        / L"talas_kernel_abi_probe_fixture.exe";
}

void drainPipe(HANDLE pipe, KernelAbiProbeResult& result)
{
    for (;;)
    {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)
            || available == 0)
        {
            return;
        }
        std::array<char, 1'024> buffer{};
        DWORD read = 0;
        if (!ReadFile(
                pipe,
                buffer.data(),
                std::min<DWORD>(available, buffer.size()),
                &read,
                nullptr)
            || read == 0)
        {
            return;
        }
        appendDiagnostic(result, buffer.data(), read);
    }
}

[[nodiscard]] KernelAbiProbeResult runPlatformProbe(
    KernelAbiProbeCase probeCase,
    std::chrono::milliseconds timeout)
{
    KernelAbiProbeResult result;
    const auto executable = probeExecutablePath();
    if (executable.empty())
    {
        result.diagnostics = "probe executable path unavailable";
        return result;
    }

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &attributes, 16'384)
        || !SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0))
    {
        result.diagnostics = "probe pipe creation failed";
        if (readPipe != nullptr)
            CloseHandle(readPipe);
        if (writePipe != nullptr)
            CloseHandle(writePipe);
        return result;
    }

    const auto argument = argumentFor(probeCase);
    std::wstring command = L"\"" + executable.wstring() + L"\" ";
    command.append(argument.begin(), argument.end());
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    const auto launched = CreateProcessW(
        executable.c_str(),
        command.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        executable.parent_path().c_str(),
        &startup,
        &process);
    CloseHandle(writePipe);
    if (!launched)
    {
        CloseHandle(readPipe);
        result.diagnostics = "probe process launch failed";
        return result;
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    bool timedOut = false;
    for (;;)
    {
        drainPipe(readPipe, result);
        if (WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0)
            break;
        if (std::chrono::steady_clock::now() >= deadline)
        {
            timedOut = true;
            TerminateProcess(process.hProcess, 124);
            WaitForSingleObject(process.hProcess, INFINITE);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    drainPipe(readPipe, result);
    CloseHandle(readPipe);
    DWORD exitCode = 0;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (timedOut)
    {
        result.outcome = KernelAbiProbeOutcome::TimedOut;
        result.exitCode = 124;
    }
    else if (isWindowsCrashStatus(exitCode))
    {
        result.outcome = KernelAbiProbeOutcome::AbnormalTermination;
        result.exitCode = static_cast<int>(exitCode);
    }
    else
    {
        classifyNormalExit(
            result,
            classificationCaseFor(probeCase),
            static_cast<int>(exitCode));
    }
    return result;
}
#else
[[nodiscard]] std::filesystem::path currentExecutablePath()
{
#if defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    if (size == 0)
        return {};
    std::vector<char> path(size);
    if (_NSGetExecutablePath(path.data(), &size) != 0)
        return {};
    return std::filesystem::path(path.data());
#else
    std::array<char, 4'096> path{};
    const auto size = readlink(
        "/proc/self/exe",
        path.data(),
        path.size() - 1);
    if (size <= 0)
        return {};
    return std::filesystem::path(
        std::string_view(path.data(), static_cast<std::size_t>(size)));
#endif
}

void drainPipe(int pipe, KernelAbiProbeResult& result)
{
    std::array<char, 1'024> buffer{};
    for (;;)
    {
        const auto readCount = read(pipe, buffer.data(), buffer.size());
        if (readCount > 0)
        {
            appendDiagnostic(
                result,
                buffer.data(),
                static_cast<std::size_t>(readCount));
            continue;
        }
        if (readCount < 0 && errno == EINTR)
            continue;
        return;
    }
}

[[nodiscard]] KernelAbiProbeResult runPlatformProbe(
    KernelAbiProbeCase probeCase,
    std::chrono::milliseconds timeout)
{
    KernelAbiProbeResult result;
    const auto current = currentExecutablePath();
    if (current.empty())
    {
        result.diagnostics = "probe executable path unavailable";
        return result;
    }
    const auto executable =
        current.parent_path() / "talas_kernel_abi_probe_fixture";
    const auto argument = argumentFor(probeCase);
    int descriptors[2] = {-1, -1};
    if (pipe(descriptors) != 0)
    {
        result.diagnostics = "probe pipe creation failed";
        return result;
    }
    const auto child = fork();
    if (child < 0)
    {
        close(descriptors[0]);
        close(descriptors[1]);
        result.diagnostics = "probe process launch failed";
        return result;
    }
    if (child == 0)
    {
        close(descriptors[0]);
        dup2(descriptors[1], STDOUT_FILENO);
        dup2(descriptors[1], STDERR_FILENO);
        close(descriptors[1]);
        execl(
            executable.c_str(),
            executable.c_str(),
            argument.data(),
            static_cast<char*>(nullptr));
        _exit(127);
    }

    close(descriptors[1]);
    const auto flags = fcntl(descriptors[0], F_GETFL, 0);
    if (flags >= 0)
        fcntl(descriptors[0], F_SETFL, flags | O_NONBLOCK);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    bool timedOut = false;
    int processStatus = 0;
    for (;;)
    {
        drainPipe(descriptors[0], result);
        const auto waited = waitpid(child, &processStatus, WNOHANG);
        if (waited == child)
            break;
        if (waited < 0 && errno != EINTR)
        {
            static constexpr std::string_view message = "probe wait failed";
            appendDiagnostic(result, message.data(), message.size());
            kill(child, SIGKILL);
            while (waitpid(child, &processStatus, 0) < 0 && errno == EINTR)
            {
            }
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
            timedOut = true;
            kill(child, SIGKILL);
            while (waitpid(child, &processStatus, 0) < 0 && errno == EINTR)
            {
            }
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    drainPipe(descriptors[0], result);
    close(descriptors[0]);
    if (timedOut)
    {
        result.outcome = KernelAbiProbeOutcome::TimedOut;
        result.exitCode = 128 + SIGKILL;
    }
    else if (WIFSIGNALED(processStatus))
    {
        result.outcome = KernelAbiProbeOutcome::AbnormalTermination;
        result.exitCode = 128 + WTERMSIG(processStatus);
    }
    else if (WIFEXITED(processStatus))
    {
        classifyNormalExit(
            result,
            classificationCaseFor(probeCase),
            WEXITSTATUS(processStatus));
    }
    else
    {
        result.outcome = KernelAbiProbeOutcome::ProtocolError;
    }
    return result;
}
#endif
}

KernelAbiProbeResult runKernelAbiProbe(
    KernelAbiProbeCase probeCase,
    std::chrono::milliseconds timeout)
{
    if (timeout <= std::chrono::milliseconds::zero())
    {
        KernelAbiProbeResult result;
        result.diagnostics = "probe timeout must be positive";
        return result;
    }
    try
    {
        return runPlatformProbe(probeCase, timeout);
    }
    catch (...)
    {
        KernelAbiProbeResult result;
        result.diagnostics = "probe coordinator failed";
        return result;
    }
}
}
