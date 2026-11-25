#include "talas/plugin/PluginPlatform.h"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#if !defined(_WIN32)
extern char** environ;
#endif

namespace talas::plugin::platform
{
std::optional<std::filesystem::path> modulePathContaining(const void* address)
{
#if defined(_WIN32)
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCWSTR>(address), &module))
    {
        return std::nullopt;
    }
    std::wstring path(32768, L'\0');
    const auto length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size())
        return std::nullopt;
    path.resize(length);
    return std::filesystem::path{path};
#else
    Dl_info info{};
    if (dladdr(address, &info) == 0 || info.dli_fname == nullptr || info.dli_fname[0] == '\0')
        return std::nullopt;
    std::error_code error;
    auto path = std::filesystem::path{info.dli_fname};
    const auto canonical = std::filesystem::weakly_canonical(path, error);
    return error ? path : canonical;
#endif
}

std::optional<std::filesystem::path> currentExecutablePath()
{
#if defined(_WIN32)
    std::wstring path(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size())
        return std::nullopt;
    path.resize(length);
    return std::filesystem::path{path};
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string path(size, '\0');
    if (size == 0 || _NSGetExecutablePath(path.data(), &size) != 0)
        return std::nullopt;
    path.resize(std::strlen(path.c_str()));
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(path, error);
    return error ? std::filesystem::path{path} : canonical;
#else
    std::error_code error;
    const auto path = std::filesystem::read_symlink("/proc/self/exe", error);
    if (error)
        return std::nullopt;
    return path;
#endif
}

std::filesystem::path bundleResourcesForBinary(const std::filesystem::path& binary)
{
    return binary.parent_path().parent_path() / "Resources";
}

bool bundlesUseInfoPlist() noexcept
{
#if defined(__APPLE__)
    return true;
#else
    return false;
#endif
}

bool supportsAudioUnits() noexcept
{
    return bundlesUseInfoPlist();
}

ProcessResult runProcess(const std::vector<std::string>& arguments)
{
    ProcessResult result;
    if (arguments.empty())
    {
        result.error = "no program to run";
        return result;
    }
#if defined(_WIN32)
    result.error = "running " + arguments.front() + " is not supported on Windows";
    return result;
#else
    int pipeEnds[2] = {-1, -1};
    if (pipe(pipeEnds) != 0)
    {
        result.error = "could not create a pipe for " + arguments.front();
        return result;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipeEnds[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipeEnds[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipeEnds[0]);
    posix_spawn_file_actions_addclose(&actions, pipeEnds[1]);
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (const auto& argument : arguments)
        argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    pid_t child = 0;
    const auto spawned = posix_spawnp(&child, argv.front(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipeEnds[1]);
    if (spawned != 0)
    {
        close(pipeEnds[0]);
        result.error = "could not run " + arguments.front() + ": " + std::strerror(spawned);
        return result;
    }
    char buffer[4096];
    while (true)
    {
        const auto count = read(pipeEnds[0], buffer, sizeof(buffer));
        if (count > 0)
        {
            result.output.append(buffer, static_cast<std::size_t>(count));
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        break;
    }
    close(pipeEnds[0]);
    int status = 0;
    while (waitpid(child, &status, 0) < 0)
    {
        if (errno != EINTR)
        {
            result.error = "could not wait for " + arguments.front();
            return result;
        }
    }
    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
#endif
}

std::string adHocSignBundle(const std::filesystem::path& bundle)
{
#if defined(__APPLE__)
    const auto signedBundle =
        runProcess({"/usr/bin/codesign", "--force", "--deep", "--sign", "-", bundle.string()});
    if (!signedBundle.ok())
    {
        return "codesign failed for " + bundle.string() + ": "
            + (signedBundle.error.empty() ? signedBundle.output : signedBundle.error);
    }
    return verifyBundleSignature(bundle);
#else
    (void)bundle;
    return {};
#endif
}

std::string verifyBundleSignature(const std::filesystem::path& bundle)
{
#if defined(__APPLE__)
    const auto verified = runProcess({"/usr/bin/codesign", "--verify", "--deep", "--strict", bundle.string()});
    if (!verified.ok())
    {
        return "codesign --verify failed for " + bundle.string() + ": "
            + (verified.error.empty() ? verified.output : verified.error);
    }
    return {};
#else
    (void)bundle;
    return {};
#endif
}

std::vector<std::filesystem::path> installedPluginFolders()
{
    std::vector<std::filesystem::path> folders;
#if defined(__APPLE__)
    if (const auto* home = std::getenv("HOME"); home != nullptr && home[0] != '\0')
    {
        folders.emplace_back(std::filesystem::path{home} / "Library/Audio/Plug-Ins/VST3");
        folders.emplace_back(std::filesystem::path{home} / "Library/Audio/Plug-Ins/Components");
    }
    folders.emplace_back("/Library/Audio/Plug-Ins/VST3");
    folders.emplace_back("/Library/Audio/Plug-Ins/Components");
#elif defined(_WIN32)
    if (const auto* common = std::getenv("COMMONPROGRAMFILES"); common != nullptr && common[0] != '\0')
        folders.emplace_back(std::filesystem::path{common} / "VST3");
#else
    if (const auto* home = std::getenv("HOME"); home != nullptr && home[0] != '\0')
        folders.emplace_back(std::filesystem::path{home} / ".vst3");
    folders.emplace_back("/usr/lib/vst3");
    folders.emplace_back("/usr/local/lib/vst3");
#endif
    return folders;
}
}
