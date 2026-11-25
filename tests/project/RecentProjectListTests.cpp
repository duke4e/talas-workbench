#include "talas/project/RecentProjectList.h"
#include "talas/project/ProjectPathEncoding.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

class TemporaryDirectory
{
public:
    TemporaryDirectory()
        : path_(
            std::filesystem::temp_directory_path()
            / "talas-recent-project-list-tests")
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        std::filesystem::create_directories(path_, error);
        if (error)
            throw std::runtime_error("Unable to create recent-project test directory");
    }

    ~TemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void writeText(const std::filesystem::path& path, std::string_view text)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}

void testMruOrderingDeduplicationNormalizationAndCapacity()
{
    talas::project::RecentProjectList list(3);
    const auto first = std::filesystem::path("sessions") / ".." / "First.talas.json";
    const auto second = std::filesystem::path("Second.talas.json");
    const auto third = std::filesystem::path("Third.talas.json");
    const auto fourth = std::filesystem::path("Fourth.talas.json");

    require(!list.note({}), "empty paths should be ignored");
    require(list.note(first), "first path should be recorded");
    require(list.note(second), "second path should be recorded");
    require(list.note(third), "third path should be recorded");
    require(list.entries().size() == 3, "list should reach configured capacity");
    require(
        list.entries()[0]
            == std::filesystem::absolute(third).lexically_normal(),
        "newest project should be first and normalized absolutely");

    require(list.note(first), "reopening an existing path should change MRU order");
    require(
        list.entries()[0]
            == std::filesystem::absolute(first).lexically_normal(),
        "existing project should move to the front");
    require(list.entries().size() == 3, "deduplication should preserve list size");

    require(list.note(fourth), "new path should be recorded at capacity");
    require(list.entries().size() == 3, "capacity should remain bounded");
    require(
        list.entries()[0]
            == std::filesystem::absolute(fourth).lexically_normal(),
        "latest path should lead after eviction");
    require(
        list.entries()[1]
            == std::filesystem::absolute(first).lexically_normal(),
        "prior MRU entry should remain second");
    require(
        !list.remove(std::filesystem::path("Not Present.talas.json")),
        "removing an absent path should report no change");
    require(list.remove(first), "removing an existing normalized path should work");
    require(list.entries().size() == 2, "removal should shrink the list");
}

void testStateRoundTripsAndReplacesAtomically()
{
    TemporaryDirectory directory;
    const auto statePath = directory.path() / "state" / "recent-projects.json";
    talas::project::RecentProjectList initial;
    const auto unicodeName = std::u8string{u8"One κ.talas.json"};
    require(initial.note(directory.path() / std::filesystem::path(unicodeName)),
            "round-trip fixture should record first path");
    require(initial.note(directory.path() / "Two.talas.json"),
            "round-trip fixture should record second path");

    require(
        talas::project::saveRecentProjectList(statePath, initial).ok(),
        "initial recent-project state should save");
    const auto loaded = talas::project::loadRecentProjectList(statePath);
    require(loaded.ok(), "saved recent-project state should load");
    require(!loaded.usedBackup, "normal load should use the primary");
    require(
        loaded.list->entries() == initial.entries(),
        "recent-project ordering and unicode paths should round-trip");
    std::ifstream persistedInput(statePath, std::ios::binary);
    const std::string persisted(
        std::istreambuf_iterator<char>{persistedInput},
        {});
    require(
        persisted.find(
            talas::project::pathToUtf8(
                std::filesystem::path(unicodeName)))
            != std::string::npos,
        "state JSON should encode native unicode paths as UTF-8");

    talas::project::RecentProjectList replacement;
    require(replacement.note(directory.path() / "Replacement.talas.json"),
            "replacement fixture should record path");
    require(
        talas::project::saveRecentProjectList(statePath, replacement).ok(),
        "replacement recent-project state should save");
    const auto replaced = talas::project::loadRecentProjectList(statePath);
    require(
        replaced.ok() && replaced.list->entries() == replacement.entries(),
        "replacement state should fully supersede the old state");
    require(
        !std::filesystem::exists(statePath.string() + ".tmp")
            && !std::filesystem::exists(statePath.string() + ".bak"),
        "successful replacement should clean sidecars");
}

void testMissingMalformedAndInvalidStateAreDistinguished()
{
    TemporaryDirectory directory;
    const auto statePath = directory.path() / "recent-projects.json";

    const auto missing = talas::project::loadRecentProjectList(statePath);
    require(missing.missing(), "missing state should be distinguishable");
    require(
        missing.error->code
            == talas::project::RecentProjectListErrorCode::NotFound,
        "missing state should report NotFound");

    writeText(statePath, "{ invalid");
    const auto malformed = talas::project::loadRecentProjectList(statePath);
    require(!malformed.ok() && !malformed.missing(), "malformed JSON should fail");
    require(
        malformed.error->code
            == talas::project::RecentProjectListErrorCode::InvalidState,
        "malformed JSON should report InvalidState");

    writeText(
        statePath,
        R"json({"format":"talas.recent-projects","version":1,"projects":[""]})json");
    const auto emptyPath = talas::project::loadRecentProjectList(statePath);
    require(!emptyPath.ok(), "persisted empty project paths should be rejected");
}

void testBackupRecoversInterruptedReplacement()
{
    TemporaryDirectory directory;
    const auto statePath = directory.path() / "recent-projects.json";
    const auto backupPath =
        std::filesystem::path(statePath.string() + ".bak");
    talas::project::RecentProjectList list;
    require(list.note(directory.path() / "Recovered.talas.json"),
            "backup fixture should record path");
    require(
        talas::project::saveRecentProjectList(statePath, list).ok(),
        "backup fixture should save");
    std::filesystem::rename(statePath, backupPath);

    const auto loaded = talas::project::loadRecentProjectList(statePath);
    require(loaded.ok() && loaded.usedBackup, "backup-only state should recover");
    require(
        loaded.list->entries() == list.entries(),
        "backup recovery should retain MRU entries");

    require(
        talas::project::saveRecentProjectList(statePath, list).ok(),
        "later save should repair backup-only state");
    require(
        std::filesystem::exists(statePath)
            && !std::filesystem::exists(backupPath),
        "repair should publish primary and remove backup");
}

void testValidPrimaryReconcilesStaleBackupOnReplacement()
{
    TemporaryDirectory directory;
    const auto statePath = directory.path() / "recent-projects.json";
    const auto backupPath =
        std::filesystem::path(statePath.string() + ".bak");
    talas::project::RecentProjectList initial;
    require(initial.note(directory.path() / "Initial.talas.json"),
            "stale-backup fixture should record initial path");
    require(talas::project::saveRecentProjectList(statePath, initial).ok(),
            "stale-backup fixture should save primary");
    std::filesystem::copy_file(statePath, backupPath);

    talas::project::RecentProjectList replacement;
    require(replacement.note(directory.path() / "Replacement.talas.json"),
            "stale-backup fixture should record replacement path");
    require(talas::project::saveRecentProjectList(statePath, replacement).ok(),
            "valid primary should make a stale regular backup disposable");
    require(!std::filesystem::exists(backupPath),
            "successful replacement should reconcile stale backup");
    const auto loaded = talas::project::loadRecentProjectList(statePath);
    require(loaded.ok() && loaded.list->entries() == replacement.entries(),
            "replacement after stale backup should publish new state");
}

void testValidBackupRecoversCorruptPrimary()
{
    TemporaryDirectory directory;
    const auto statePath = directory.path() / "recent-projects.json";
    const auto backupPath =
        std::filesystem::path(statePath.string() + ".bak");
    talas::project::RecentProjectList backup;
    require(backup.note(directory.path() / "Backup.talas.json"),
            "corrupt-primary fixture should record backup path");
    require(talas::project::saveRecentProjectList(backupPath, backup).ok(),
            "corrupt-primary fixture should save backup state");
    writeText(statePath, "{ corrupt primary");

    const auto loaded = talas::project::loadRecentProjectList(statePath);
    require(loaded.ok() && loaded.usedBackup,
            "valid backup should recover a corrupt primary");
    require(loaded.list->entries() == backup.entries(),
        "backup fallback should preserve its MRU entries");
}

void testFuturePrimaryDoesNotRollBackToOlderBackup()
{
    TemporaryDirectory directory;
    const auto statePath = directory.path() / "recent-projects.json";
    const auto backupPath =
        std::filesystem::path(statePath.string() + ".bak");
    talas::project::RecentProjectList backup;
    require(backup.note(directory.path() / "Old.talas.json"),
            "future-version fixture should record backup path");
    require(talas::project::saveRecentProjectList(backupPath, backup).ok(),
            "future-version fixture should save v1 backup");
    const std::string futureState =
        R"json({"format":"talas.recent-projects","version":2,"projects":[]})json";
    writeText(statePath, futureState);

    const auto loaded = talas::project::loadRecentProjectList(statePath);
    require(!loaded.ok() && !loaded.usedBackup,
            "unsupported future primary must not fall back to older state");
    require(
        loaded.error->code
            == talas::project::RecentProjectListErrorCode::UnsupportedState,
        "future primary should report UnsupportedState");
    std::ifstream primaryInput(statePath, std::ios::binary);
    const std::string primary(
        std::istreambuf_iterator<char>{primaryInput},
        {});
    require(primary == futureState,
            "future primary must remain byte-for-byte intact");
}

void testTemporarySymlinkNeverTruncatesItsTarget()
{
    TemporaryDirectory directory;
    const auto statePath = directory.path() / "recent-projects.json";
    const auto temporaryPath =
        std::filesystem::path(statePath.string() + ".tmp");
    const auto unrelatedPath = directory.path() / "unrelated.txt";
    writeText(unrelatedPath, "must remain unchanged");
    std::filesystem::create_symlink(unrelatedPath, temporaryPath);
    talas::project::RecentProjectList list;
    require(list.note(directory.path() / "Project.talas.json"),
            "temporary-symlink fixture should record path");

    require(talas::project::saveRecentProjectList(statePath, list).ok(),
            "removable temporary symlink should be replaced safely");
    std::ifstream unrelatedInput(unrelatedPath, std::ios::binary);
    const std::string unrelated(
        std::istreambuf_iterator<char>{unrelatedInput},
        {});
    require(unrelated == "must remain unchanged",
            "temporary symlink target must never be opened or truncated");
    require(!std::filesystem::is_symlink(temporaryPath),
            "temporary symlink should not survive successful save");
}

void testNonRegularDestinationIsRejected()
{
    TemporaryDirectory directory;
    const auto statePath = directory.path() / "recent-projects.json";
    std::filesystem::create_directory(statePath);
    talas::project::RecentProjectList list;
    require(list.note(directory.path() / "Project.talas.json"),
            "unsafe-destination fixture should record path");

    const auto saved = talas::project::saveRecentProjectList(statePath, list);
    require(!saved.ok(), "directory destination should be rejected");
    require(
        saved.error->code
            == talas::project::RecentProjectListErrorCode::WriteFailed,
        "unsafe destination should report WriteFailed");
    require(
        std::filesystem::is_directory(statePath),
        "rejected destination directory should remain intact");
}
}

int main()
{
    testMruOrderingDeduplicationNormalizationAndCapacity();
    testStateRoundTripsAndReplacesAtomically();
    testMissingMalformedAndInvalidStateAreDistinguished();
    testBackupRecoversInterruptedReplacement();
    testValidPrimaryReconcilesStaleBackupOnReplacement();
    testValidBackupRecoversCorruptPrimary();
    testFuturePrimaryDoesNotRollBackToOlderBackup();
    testTemporarySymlinkNeverTruncatesItsTarget();
    testNonRegularDestinationIsRejected();
    return 0;
}
