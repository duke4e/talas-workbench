#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace talas::app
{
enum class WorkbenchLaunchMode
{
    Normal,
    SmokeTest
};

struct WorkbenchLaunchOptions
{
    WorkbenchLaunchMode mode = WorkbenchLaunchMode::Normal;
    std::filesystem::path smokeDirectory;
    // The preferences file the Workbench was started
    // by opening, a user's own, read and kept instead of the shared one.
    std::filesystem::path preferencesFile;
};

// The preferences are kept in talas.pre, created the
// first time the Workbench quits; each user may keep a copy of it in a
// folder of their own and start the Workbench by double-clicking it.
inline constexpr std::string_view workbenchPreferencesFileName{"talas.pre"};
[[nodiscard]] bool isWorkbenchPreferencesFile(const std::filesystem::path& path);
// The preferences file a file opened while the Workbench runs names (the
// command line the system passes, the path perhaps in quotes), if it is
// one that exists.
[[nodiscard]] std::optional<std::filesystem::path> preferencesFileOpened(std::string_view commandLine);

struct WorkbenchLaunchParseResult
{
    std::optional<WorkbenchLaunchOptions> options;
    std::string error;

    [[nodiscard]] bool ok() const noexcept
    {
        return options.has_value();
    }
};

[[nodiscard]] WorkbenchLaunchParseResult parseWorkbenchLaunch(
    const std::vector<std::string>& arguments);

[[nodiscard]] bool requiresIndependentWorkbenchInstance(
    const std::vector<std::string>& arguments) noexcept;
}
