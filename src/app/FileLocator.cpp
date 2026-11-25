#include "talas/app/FileLocator.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <system_error>

namespace talas::app
{
namespace
{
[[nodiscard]] bool isFile(const std::filesystem::path& path)
{
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
}

// The file of that name in the folder or any folder inside it, the
// shallowest and then the first in name order.
[[nodiscard]] std::optional<std::filesystem::path> findWithin(const std::filesystem::path& folder, const std::filesystem::path& name,
    const std::function<void(std::size_t)>& searching, std::size_t& searched, std::size_t budget, bool& stopped)
{
    if (isFile(folder / name))
        return folder / name;
    std::error_code error;
    std::vector<std::filesystem::path> found;
    for (std::filesystem::recursive_directory_iterator entry{
             folder, std::filesystem::directory_options::skip_permission_denied, error};
         !error && entry != std::filesystem::recursive_directory_iterator{}; entry.increment(error))
    {
        // Its budget spent, the search stops to ask.
        if (searched >= budget)
        {
            stopped = true;
            break;
        }
        if (++searched % 64 == 0 && searching)
            searching(searched);
        if (entry->path().filename() == name && isFile(entry->path()))
            found.push_back(entry->path());
    }
    if (found.empty())
        return std::nullopt;
    std::ranges::sort(found, [](const auto& left, const auto& right) {
        const auto leftDepth = std::distance(left.begin(), left.end());
        const auto rightDepth = std::distance(right.begin(), right.end());
        return leftDepth != rightDepth ? leftDepth < rightDepth : left < right;
    });
    return found.front();
}
}

void rememberLocatedFile(const std::filesystem::path& path, FileLocatorMemory& memory)
{
    memory.recentlyFound[path.filename().string()] = path;
    const auto folder = path.parent_path();
    std::erase(memory.recentFolders, folder);
    memory.recentFolders.insert(memory.recentFolders.begin(), folder);
    if (memory.recentFolders.size() > maximumRecentFileFolders)
        memory.recentFolders.resize(maximumRecentFileFolders);
}

void forgetLocatedFiles(FileLocatorMemory& memory)
{
    memory = {};
}

std::string fileNotFoundQuestion(std::string_view name)
{
    return "Talas could not yet locate " + std::string{name} + ". Keep looking, use a file dialog, or cancel?";
}

std::vector<std::string> fileSearchChoices()
{
    return {"Keep looking", "Use file dialog", "Cancel"};
}

FileSearchAnswer fileSearchAfterAnotherLook(bool searchedAgain, FileSearchAnswer answer) noexcept
{
    return searchedAgain && answer == FileSearchAnswer::KeepLooking ? FileSearchAnswer::UseFileDialog : answer;
}

std::optional<LocatedFile> locateFile(
    std::string_view name,
    const std::vector<std::filesystem::path>& frequentlyUsedFolders,
    FileLocatorMemory& memory,
    const std::function<void(std::size_t searched)>& searching)
{
    return searchForFile(name, frequentlyUsedFolders, memory, std::numeric_limits<std::size_t>::max(), searching).located;
}

FileSearchOutcome searchForFile(
    std::string_view name,
    const std::vector<std::filesystem::path>& frequentlyUsedFolders,
    FileLocatorMemory& memory,
    std::size_t budget,
    const std::function<void(std::size_t searched)>& searching)
{
    const std::filesystem::path given{std::string{name}};
    if (given.empty())
        return {};
    const auto found = [&](std::filesystem::path path, FileLocation location) {
        rememberLocatedFile(path, memory);
        return FileSearchOutcome{LocatedFile{std::move(path), location}, true};
    };
    if (given.is_absolute() && isFile(given))
        return found(given, FileLocation::CompletePath);
    const auto alone = given.filename();
    if (alone.empty())
        return {};
    if (const auto recent = memory.recentlyFound.find(alone.string());
        recent != memory.recentlyFound.end() && isFile(recent->second))
        return found(recent->second, FileLocation::RecentlyFound);
    // Remembering a file reorders the recent folders: copy the one found.
    const auto recentFolder = std::ranges::find_if(
        memory.recentFolders, [&](const auto& folder) { return isFile(folder / alone); });
    if (recentFolder != memory.recentFolders.end())
        return found(std::filesystem::path{*recentFolder} / alone, FileLocation::RecentFolder);
    std::size_t searched = 0;
    bool stopped = false;
    for (const auto& folder : frequentlyUsedFolders)
    {
        if (auto within = findWithin(folder, alone, searching, searched, budget, stopped))
            return found(std::move(*within), FileLocation::FrequentlyUsedFolder);
        if (stopped)
            return {std::nullopt, false};
    }
    return {};
}

FileSearchStep fileSearchStep(const FileSearchOutcome& outcome, bool keptLooking) noexcept
{
    if (outcome.located.has_value())
        return FileSearchStep::Use;
    return keptLooking && outcome.exhausted ? FileSearchStep::FileList : FileSearchStep::Ask;
}
}
