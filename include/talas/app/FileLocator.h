#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// How a file a Sound names is located. A
// name with a complete path to a file that exists is used as it is;
// otherwise the path is dropped and the name looked for, in order, among
// the files found recently, in the folders used most recently, and in each
// frequently used folder and the folders inside it. Any of the files of
// that name may be the one found.
namespace talas::app
{
inline constexpr std::size_t maximumRecentFileFolders = 8;

struct FileLocatorMemory
{
    // A name, by itself, and the file it was last found as.
    std::map<std::string, std::filesystem::path, std::less<>> recentlyFound;
    // The folders files were last found in, the latest first.
    std::vector<std::filesystem::path> recentFolders;
};

enum class FileLocation
{
    CompletePath,
    RecentlyFound,
    RecentFolder,
    FrequentlyUsedFolder,
};

struct LocatedFile
{
    std::filesystem::path path;
    FileLocation location = FileLocation::CompletePath;
};

// The file a name stands for, remembered for next time; none when no
// folder holds it. Searching the folders, it tells `searching` how many
// entries it has looked at, every 64 (the cursor's eyeglasses move as the
// search goes on).
[[nodiscard]] std::optional<LocatedFile> locateFile(
    std::string_view name,
    const std::vector<std::filesystem::path>& frequentlyUsedFolders,
    FileLocatorMemory& memory,
    const std::function<void(std::size_t searched)>& searching = {});
// The search looks through the folders for some time,
// then asks whether to keep looking. The search looks at `budget` entries
// of the frequently used folders at most: `exhausted` tells whether it saw
// them all, else it stopped to ask. Keep looking searches again with a
// larger budget.
struct FileSearchOutcome
{
    std::optional<LocatedFile> located;
    bool exhausted = true;
};
[[nodiscard]] FileSearchOutcome searchForFile(
    std::string_view name,
    const std::vector<std::filesystem::path>& frequentlyUsedFolders,
    FileLocatorMemory& memory,
    std::size_t budget,
    const std::function<void(std::size_t searched)>& searching = {});
// What follows a search: the file bound, the question asked (Keep looking,
// Use file dialog, Cancel), or, once Keep looking has looked everywhere in
// vain, the file list.
enum class FileSearchStep
{
    Use,
    Ask,
    FileList,
};
[[nodiscard]] FileSearchStep fileSearchStep(const FileSearchOutcome& outcome, bool keptLooking) noexcept;
// A file used or found: its name and folder are the most recent.
void rememberLocatedFile(const std::filesystem::path& path, FileLocatorMemory& memory);
// A file no folder holds asks Keep looking (the
// folders searched again), Use file dialog (a file list) or Cancel; a
// second search that finds nothing goes on to the file list, every
// automatic way exhausted.
enum class FileSearchAnswer
{
    KeepLooking,
    UseFileDialog,
    Cancel,
};

[[nodiscard]] std::string fileNotFoundQuestion(std::string_view name);
[[nodiscard]] std::vector<std::string> fileSearchChoices();
[[nodiscard]] FileSearchAnswer fileSearchAfterAnotherLook(bool searchedAgain, FileSearchAnswer answer) noexcept;

// The Status window's Recycle forgets the files and
// folders found recently, after files were changed by another application.
void forgetLocatedFiles(FileLocatorMemory& memory);
}
