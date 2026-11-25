#include "talas/app/WorkbenchLaunch.h"

#include <algorithm>
#include <string_view>
#include <system_error>

namespace talas::app
{
namespace
{
constexpr auto usage =
    "usage: talas_workbench [--smoke-test <absolute-existing-directory> | <preferences-file.pre>]";

std::filesystem::path pathFromUtf8(std::string_view value)
{
    std::u8string encoded;
    encoded.reserve(value.size());
    for (const auto byte : value)
        encoded.push_back(static_cast<char8_t>(
            static_cast<unsigned char>(byte)));
    return std::filesystem::path(encoded);
}
}

bool isWorkbenchPreferencesFile(const std::filesystem::path& path)
{
    auto extension = path.extension().u8string();
    std::ranges::transform(extension, extension.begin(), [](char8_t character) {
        return character >= u8'A' && character <= u8'Z' ? static_cast<char8_t>(character - u8'A' + u8'a') : character;
    });
    return extension == u8".pre";
}

std::optional<std::filesystem::path> preferencesFileOpened(std::string_view commandLine)
{
    while (!commandLine.empty() && (commandLine.front() == ' ' || commandLine.front() == '"'))
        commandLine.remove_prefix(1);
    while (!commandLine.empty() && (commandLine.back() == ' ' || commandLine.back() == '"'))
        commandLine.remove_suffix(1);
    auto path = pathFromUtf8(commandLine);
    std::error_code error;
    if (commandLine.empty() || !isWorkbenchPreferencesFile(path) || !std::filesystem::is_regular_file(path, error))
        return std::nullopt;
    return std::filesystem::absolute(path, error).lexically_normal();
}

WorkbenchLaunchParseResult parseWorkbenchLaunch(
    const std::vector<std::string>& arguments)
{
    if (arguments.empty())
        return {WorkbenchLaunchOptions{}, {}};

    // Started by double-clicking a user's preferences file.
    if (arguments.size() == 1)
    {
        if (auto preferences = preferencesFileOpened(arguments.front()))
        {
            WorkbenchLaunchOptions options;
            options.preferencesFile = std::move(*preferences);
            return {std::move(options), {}};
        }
    }

    if (arguments.size() != 2 || arguments.front() != "--smoke-test")
        return {{}, usage};

    auto directory = pathFromUtf8(arguments[1]);
    if (!directory.is_absolute())
        return {{}, "smoke-test directory must be absolute; " + std::string(usage)};

    directory = directory.lexically_normal();
    std::error_code error;
    const auto isDirectory = std::filesystem::is_directory(directory, error);
    if (error || !isDirectory)
        return {{}, "smoke-test path must be an existing directory"};

    // Smoke runs write dozens of screenshots, takes and packages; a
    // repository root would receive them as untracked clutter.
    const auto repositoryMarker = std::filesystem::symlink_status(
        directory / ".git", error);
    if (!error && std::filesystem::exists(repositoryMarker))
        return {{}, "smoke-test directory must not be a repository root; use a dedicated subdirectory"};

    WorkbenchLaunchOptions options;
    options.mode = WorkbenchLaunchMode::SmokeTest;
    options.smokeDirectory = std::move(directory);
    return {std::move(options), {}};
}

bool requiresIndependentWorkbenchInstance(
    const std::vector<std::string>& arguments) noexcept
{
    return std::ranges::find(arguments, "--smoke-test")
        != arguments.end();
}
}
