#include "talas/app/FileLocator.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// How a file named by a Sound is located.
namespace
{
using talas::app::FileLocation;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void touch(const std::filesystem::path& path)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream{path} << "x";
}

void testTheOrderOfTheSearch()
{
    const auto root = std::filesystem::temp_directory_path() / "talas_file_locator_tests";
    std::filesystem::remove_all(root);
    const auto samples = root / "Samples";
    const auto other = root / "Other";
    touch(samples / "Drums" / "Kits" / "kick.wav");
    touch(samples / "Drums" / "snare.wav");
    touch(other / "snare.wav");
    touch(root / "elsewhere" / "pad.aif");
    const std::vector<std::filesystem::path> frequent{samples, other};

    talas::app::FileLocatorMemory memory;
    // A complete path to a file that exists is used directly.
    const auto direct = talas::app::locateFile((root / "elsewhere" / "pad.aif").string(), frequent, memory);
    require(direct && direct->path == root / "elsewhere" / "pad.aif" && direct->location == FileLocation::CompletePath,
        "a complete path to a file that exists should be used as it is");
    // The path is dropped; each frequently used folder and the folders in
    // it are searched for the name.
    const auto kick = talas::app::locateFile("/Volumes/Gone/kick.wav", frequent, memory);
    require(kick && kick->path == samples / "Drums" / "Kits" / "kick.wav"
            && kick->location == FileLocation::FrequentlyUsedFolder,
        "a name should be found in a frequently used folder's subfolders, its old path dropped");
    // Found recently, the same file is used again.
    const auto again = talas::app::locateFile("kick.wav", frequent, memory);
    require(again && again->path == kick->path && again->location == FileLocation::RecentlyFound,
        "a name found recently should be the file found then");
    // The most recently used folders are checked before the frequently
    // used ones: snare.wav in Other is found next to the pad used last.
    touch(root / "elsewhere" / "snare.wav");
    const auto snare = talas::app::locateFile("snare.wav", frequent, memory);
    require(snare && snare->path == root / "elsewhere" / "snare.wav" && snare->location == FileLocation::RecentFolder,
        "the most recently used folders should be checked before the frequently used folders");
    // Any file of the name may be the one: the first frequently used folder
    // that holds one, then the shallowest.
    talas::app::FileLocatorMemory fresh;
    std::filesystem::remove(root / "elsewhere" / "snare.wav");
    const auto first = talas::app::locateFile("snare.wav", frequent, fresh);
    require(first && first->path == samples / "Drums" / "snare.wav",
        "of several files of the name, the first frequently used folder's should be found");
    require(!talas::app::locateFile("missing.wav", frequent, fresh) && !talas::app::locateFile("", frequent, fresh),
        "a name no folder holds is not found");
    // The search tells how far it has looked, so the
    // cursor's eyeglasses can move as it goes.
    for (int index = 0; index < 200; ++index)
        touch(samples / "Many" / ("file" + std::to_string(index) + ".wav"));
    std::vector<std::size_t> looked;
    talas::app::FileLocatorMemory searchingMemory;
    (void)talas::app::locateFile("nowhere.wav", frequent, searchingMemory, [&](std::size_t searched) { looked.push_back(searched); });
    require(looked.size() >= 3 && std::ranges::is_sorted(looked) && looked.front() == 64,
        "the search should report its progress as it looks through the folders");
    for (std::size_t index = 0; index < talas::app::maximumRecentFileFolders + 3; ++index)
        talas::app::rememberLocatedFile(root / ("f" + std::to_string(index)) / "a.wav", fresh);
    require(fresh.recentFolders.size() == talas::app::maximumRecentFileFolders
            && fresh.recentFolders.front() == root / ("f" + std::to_string(talas::app::maximumRecentFileFolders + 2)),
        "the recent folders should keep the latest first, and only so many");
    // After another application changed the files, Recycle
    // forgets what was found, and the frequently used folders are searched.
    talas::app::FileLocatorMemory changed;
    touch(root / "elsewhere" / "snare.wav");
    talas::app::rememberLocatedFile(root / "elsewhere" / "snare.wav", changed);
    require(talas::app::locateFile("snare.wav", frequent, changed)->location == FileLocation::RecentlyFound,
        "the file found recently should be found again");
    talas::app::forgetLocatedFiles(changed);
    const auto recycled = talas::app::locateFile("snare.wav", frequent, changed);
    require(recycled && recycled->path == samples / "Drums" / "snare.wav"
            && recycled->location == FileLocation::FrequentlyUsedFolder,
        "after Recycle, the name should be looked for in the frequently used folders again");
    std::filesystem::remove_all(root);
}

// A file no folder holds asks Keep looking, Use
// file dialog or Cancel; looked for again in vain, the file list opens.
void testAFileNotFoundAsks()
{
    using talas::app::FileSearchAnswer;
    require(talas::app::fileNotFoundQuestion("hat.wav").find("hat.wav") != std::string::npos
            && talas::app::fileSearchChoices() == std::vector<std::string>{"Keep looking", "Use file dialog", "Cancel"},
        "the dialog should name the file and offer Keep looking, Use file dialog and Cancel");
    require(talas::app::fileSearchAfterAnotherLook(false, FileSearchAnswer::KeepLooking) == FileSearchAnswer::KeepLooking
            && talas::app::fileSearchAfterAnotherLook(true, FileSearchAnswer::KeepLooking) == FileSearchAnswer::UseFileDialog
            && talas::app::fileSearchAfterAnotherLook(true, FileSearchAnswer::Cancel) == FileSearchAnswer::Cancel,
        "once the folders were searched again in vain, the file list should open");
}
}

// The locator searches the folders for some time and
// then asks whether to keep looking, again and again while it searches;
// Keep looking goes on searching, and once it has looked everywhere in
// vain, the file list opens.
void testTheSearchStopsToAsk()
{
    const auto root = std::filesystem::temp_directory_path() / "talas_file_search_budget_tests";
    std::filesystem::remove_all(root);
    for (int folder = 0; folder < 20; ++folder)
        for (int file = 0; file < 10; ++file)
            touch(root / ("Folder" + std::to_string(folder)) / ("take" + std::to_string(file) + ".wav"));
    touch(root / "Zebra" / "Deep" / "wanted.wav");
    const std::vector<std::filesystem::path> frequent{root};
    using talas::app::FileSearchStep;
    talas::app::FileLocatorMemory memory;
    const auto hurried = talas::app::searchForFile("wanted.wav", frequent, memory, 2);
    require(!hurried.located.has_value() && !hurried.exhausted && talas::app::fileSearchStep(hurried, false) == FileSearchStep::Ask
            && talas::app::fileSearchStep(hurried, true) == FileSearchStep::Ask,
        "a search that has looked for some time in vain should stop to ask, and ask again while it has more to see");
    const auto further = talas::app::searchForFile("wanted.wav", frequent, memory, 100000);
    require(further.located.has_value() && further.located->path == root / "Zebra" / "Deep" / "wanted.wav"
            && talas::app::fileSearchStep(further, true) == FileSearchStep::Use,
        "keeping on looking should find the file further on");
    const auto nowhere = talas::app::searchForFile("missing.wav", frequent, memory, 100000);
    require(!nowhere.located.has_value() && nowhere.exhausted && talas::app::fileSearchStep(nowhere, false) == FileSearchStep::Ask
            && talas::app::fileSearchStep(nowhere, true) == FileSearchStep::FileList,
        "once every folder was looked through in vain, keeping on looking should open the file list");
    std::filesystem::remove_all(root);
}

int main()
{
    try
    {
        testTheOrderOfTheSearch();
        testAFileNotFoundAsks();
        testTheSearchStopsToAsk();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
