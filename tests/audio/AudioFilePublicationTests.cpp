#include "talas/audio/AudioFilePublication.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

std::filesystem::path temporaryDirectory()
{
    static int sequence = 0;
    auto path = std::filesystem::temp_directory_path()
        / ("talas-audio-publication-test-"
           + std::to_string(std::chrono::steady_clock::now()
                                .time_since_epoch().count())
           + "-" + std::to_string(sequence++));
    require(std::filesystem::create_directory(path),
        "test should create a private directory");
    return path;
}

void writeText(const std::filesystem::path& path, std::string_view text)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    require(output.good(), "test should write fixture bytes");
}

std::string readText(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
}

void testPublishesNewFileFromPrivateStaging()
{
    const auto directory = temporaryDirectory();
    const auto destination = directory / "take.wav";
    std::string error;
    auto publication = talas::audio::AudioFilePublication::prepare(
        destination,
        talas::audio::ExistingAudioFilePolicy::Reject,
        error);
    require(publication != nullptr, "new destination should prepare: " + error);
    const auto stagingDirectory = publication->temporaryPath().parent_path();
    require(stagingDirectory != directory
            && stagingDirectory.parent_path()
                == std::filesystem::weakly_canonical(directory),
        "temporary file should live in one private sibling directory");
    writeText(publication->temporaryPath(), "exact take");
    require(publication->publish(error),
        "new destination should publish atomically: " + error);
    require(readText(destination) == "exact take",
        "published destination should contain staged bytes");
    require(!std::filesystem::exists(stagingDirectory),
        "successful publication should clean private staging");
    std::filesystem::remove(destination);
    std::filesystem::remove(directory);
}

void testExistingFileRequiresExplicitReplacement()
{
    const auto directory = temporaryDirectory();
    const auto destination = directory / "take.wav";
    writeText(destination, "original");
    std::string error;
    auto rejected = talas::audio::AudioFilePublication::prepare(
        destination,
        talas::audio::ExistingAudioFilePolicy::Reject,
        error);
    require(rejected == nullptr
            && error.find("explicit replacement") != std::string::npos,
        "existing destination should fail without owner replacement policy");
    require(readText(destination) == "original",
        "rejected preparation must preserve existing bytes");

    auto replacement = talas::audio::AudioFilePublication::prepare(
        destination,
        talas::audio::ExistingAudioFilePolicy::Replace,
        error);
    require(replacement != nullptr,
        "explicit replacement should prepare: " + error);
    writeText(replacement->temporaryPath(), "replacement");
    writeText(destination, "concurrent owner revision");
    require(replacement->publish(error),
        "explicit replacement should publish: " + error);
    require(readText(destination) == "replacement",
        "explicit replacement should atomically install exact staged bytes even when the destination changes after preparation");
    std::filesystem::remove(destination);
    std::filesystem::remove(directory);
}

void testCancelRemovesOnlyPrivateTemporaryFile()
{
    const auto directory = temporaryDirectory();
    const auto destination = directory / "take.wav";
    const auto neighbor = directory / "neighbor.txt";
    writeText(neighbor, "keep");
    std::string error;
    auto publication = talas::audio::AudioFilePublication::prepare(
        destination,
        talas::audio::ExistingAudioFilePolicy::Reject,
        error);
    require(publication != nullptr, "cancel fixture should prepare");
    const auto temporary = publication->temporaryPath();
    const auto staging = temporary.parent_path();
    writeText(temporary, "partial");
    require(publication->discard(error),
        "cancel should remove exact private staging: " + error);
    require(!std::filesystem::exists(destination)
            && !std::filesystem::exists(temporary)
            && !std::filesystem::exists(staging),
        "cancel should not publish or retain private staging");
    require(readText(neighbor) == "keep",
        "cancel must not remove an unrelated sibling");
    std::filesystem::remove(neighbor);
    std::filesystem::remove(directory);
}

void testDestructorRemovesUnpublishedStaging()
{
    const auto directory = temporaryDirectory();
    const auto destination = directory / "take.wav";
    std::string error;
    auto publication = talas::audio::AudioFilePublication::prepareContained(
        directory,
        "take.wav",
        talas::audio::ExistingAudioFilePolicy::Reject,
        error);
    require(publication != nullptr,
        "destructor cleanup fixture should prepare: " + error);
    const auto stagingDirectory =
        publication->temporaryPath().parent_path();
    auto* output = publication->openTemporaryFile(error);
    require(output != nullptr,
        "destructor cleanup fixture should open staging: " + error);
    require(std::fwrite("partial", 1, 7, output) == 7
            && std::fclose(output) == 0,
        "destructor cleanup fixture should persist partial bytes");

    publication.reset();
    require(!std::filesystem::exists(stagingDirectory)
            && !std::filesystem::exists(destination),
        "unpublished destruction must remove all private staging artifacts");
    std::filesystem::remove(directory);
}

void testDestinationRaceAndSymlinkFailClosed()
{
    const auto directory = temporaryDirectory();
    const auto destination = directory / "race.wav";
    std::string error;
    auto publication = talas::audio::AudioFilePublication::prepare(
        destination,
        talas::audio::ExistingAudioFilePolicy::Reject,
        error);
    require(publication != nullptr, "race fixture should prepare");
    writeText(publication->temporaryPath(), "private take");
    writeText(destination, "concurrent owner file");
    require(!publication->publish(error)
            && error.find("appeared before publication")
                != std::string::npos,
        "destination appearing after preparation must fail closed");
    require(readText(destination) == "concurrent owner file",
        "destination race must preserve the independently created file");
    require(publication->discard(error),
        "failed publication should still clean private staging: " + error);
    std::filesystem::remove(destination);

    const auto target = directory / "target.wav";
    const auto symlink = directory / "link.wav";
    writeText(target, "target");
    std::error_code symlinkError;
    std::filesystem::create_symlink(target.filename(), symlink, symlinkError);
    if (!symlinkError)
    {
        auto rejected = talas::audio::AudioFilePublication::prepare(
            symlink,
            talas::audio::ExistingAudioFilePolicy::Replace,
            error);
        require(rejected == nullptr
                && error.find("regular file") != std::string::npos,
            "symlink destination must reject even with replace policy");
        require(readText(target) == "target",
            "symlink rejection must preserve its target");
        std::filesystem::remove(symlink);
    }
    std::filesystem::remove(target);
    std::filesystem::remove(directory);
}

void testProtectedDestinationRelationships()
{
    const auto directory = temporaryDirectory();
    const auto package = directory / "Project.talaspkg";
    const auto assets = package / "Assets";
    require(std::filesystem::create_directories(assets),
        "protected relationship fixture should create package assets");
    const auto project = package / "Project.talas.json";
    const auto source = assets / "source.wav";
    writeText(project, "project");
    writeText(source, "source");
    const std::array protectedFiles{project, source};
    const std::array protectedDirectories{package};

    require(talas::audio::audioFileDestinationConflicts(
                project,
                protectedFiles,
                {}),
        "output must not alias the open project file");
    require(talas::audio::audioFileDestinationConflicts(
                source.parent_path() / "." / source.filename(),
                protectedFiles,
                {}),
        "normalized output must not alias a protected source asset");
    const auto hardLinkAlias = directory / "source-alias.wav";
    std::error_code hardLinkError;
    std::filesystem::create_hard_link(
        source,
        hardLinkAlias,
        hardLinkError);
    if (!hardLinkError)
    {
        require(talas::audio::audioFileDestinationConflicts(
                    hardLinkAlias,
                    protectedFiles,
                    {}),
            "filesystem-equivalent output must not alias a protected source asset");
        std::filesystem::remove(hardLinkAlias);
    }
    require(talas::audio::audioFileDestinationConflicts(
                assets / "new-take.wav",
                {},
                protectedDirectories),
        "output must not mutate the open project package");
    require(!talas::audio::audioFileDestinationConflicts(
                directory / "safe-take.wav",
                protectedFiles,
                protectedDirectories),
        "unrelated sibling output should remain available");

    std::filesystem::remove(source);
    std::filesystem::remove(project);
    std::filesystem::remove(assets);
    std::filesystem::remove(package);
    std::filesystem::remove(directory);
}

void testAncestorSwapCannotRedirectHeldPublication()
{
    const auto directory = temporaryDirectory();
    const auto cacheDirectory = directory / "DiskCache";
    require(std::filesystem::create_directory(cacheDirectory),
        "ancestor-swap fixture should create contained directory");
    const auto destination = cacheDirectory / "held.wav";
    std::string error;
    auto publication = talas::audio::AudioFilePublication::prepareContained(
        directory,
        std::filesystem::path{"DiskCache"} / "held.wav",
        talas::audio::ExistingAudioFilePolicy::Reject,
        error);
    require(publication != nullptr, "held publication should prepare: " + error);
    auto* output = publication->openTemporaryFile(error);
    require(output != nullptr, "held staging file should open: " + error);
    constexpr std::string_view bytes{"held bytes"};
    require(
        std::fwrite(bytes.data(), 1, bytes.size(), output) == bytes.size()
            && std::fclose(output) == 0,
        "held staging stream should persist exact bytes");

    const auto displaced = directory / "DiskCache-displaced";
#if defined(_WIN32)
    std::error_code renameError;
    std::filesystem::rename(cacheDirectory, displaced, renameError);
    require(renameError,
        "Windows held ancestor authority must deny directory replacement");
    require(publication->publish(error),
        "Windows held publication should publish without an ancestor race: " + error);
    require(readText(destination) == "held bytes",
        "Windows held directory authority should publish exact bytes");
    publication.reset();
    std::filesystem::remove(destination);
    std::filesystem::remove(cacheDirectory);
    std::filesystem::remove(directory);
    return;
#else
    std::filesystem::rename(cacheDirectory, displaced);
    require(std::filesystem::create_directory(cacheDirectory),
        "ancestor-swap fixture should install a replacement directory");
    writeText(destination, "attacker bytes");

    require(
        publication->publish(error),
        "publication should remain bound to the held directory object: " + error);
    require(
        readText(destination) == "attacker bytes",
        "ancestor swap must not redirect publication into the replacement path");
    require(
        readText(displaced / "held.wav") == "held bytes",
        "held directory authority must receive the exact staged bytes");

    std::filesystem::remove(destination);
    std::filesystem::remove(cacheDirectory);
    std::filesystem::remove(displaced / "held.wav");
    std::filesystem::remove(displaced);
    std::filesystem::remove(directory);
#endif
}

void testVerifiedStagingLeafSwapIsRejected()
{
    const auto directory = temporaryDirectory();
    const auto destination = directory / "take.wav";
    std::string error;
    auto publication = talas::audio::AudioFilePublication::prepareContained(
        directory,
        "take.wav",
        talas::audio::ExistingAudioFilePolicy::Reject,
        error);
    require(publication != nullptr,
        "leaf-swap publication should prepare: " + error);
    auto* output = publication->openTemporaryFile(error);
    require(output != nullptr,
        "leaf-swap staging file should open: " + error);
    constexpr std::string_view verifiedBytes{"verified bytes"};
    require(
        std::fwrite(
            verifiedBytes.data(), 1, verifiedBytes.size(), output)
                == verifiedBytes.size()
            && std::fclose(output) == 0,
        "leaf-swap fixture should persist verified bytes");
    std::vector<std::uint8_t> snapshot;
    require(publication->readTemporaryFile(1024U, snapshot, error)
            && std::string{snapshot.begin(), snapshot.end()} == verifiedBytes,
        "leaf-swap fixture should bind the inspected staging inode");

    const auto verifiedLeaf =
        publication->temporaryPath().parent_path() / "verified-leaf.wav";
#if defined(_WIN32)
    std::error_code renameError;
    std::filesystem::rename(
        publication->temporaryPath(), verifiedLeaf, renameError);
    require(renameError,
        "Windows verified staging handle must deny leaf replacement");
    require(publication->publish(error),
        "Windows verified staging leaf should publish exact bytes: " + error);
    require(readText(destination) == verifiedBytes,
        "Windows publication must retain the verified bytes");
    publication.reset();
    std::filesystem::remove(destination);
    std::filesystem::remove(directory);
    return;
#else
    std::filesystem::rename(publication->temporaryPath(), verifiedLeaf);
    writeText(publication->temporaryPath(), "attacker bytes");

    require(!publication->publish(error),
        "replacing the verified staging leaf must reject publication");
    require(!std::filesystem::exists(destination),
        "rejected leaf swap must not publish a destination");
    require(readText(publication->temporaryPath()) == "attacker bytes",
        "rejected leaf swap must not delete the replacement file");

    const auto stagingDirectory =
        publication->temporaryPath().parent_path();
    const auto replacementLeaf = publication->temporaryPath();
    publication.reset();
    require(readText(replacementLeaf) == "attacker bytes"
            && readText(verifiedLeaf) == verifiedBytes,
        "destruction after a leaf swap must preserve both unowned names");

    std::filesystem::remove(verifiedLeaf);
    std::filesystem::remove(replacementLeaf);
    std::filesystem::remove(stagingDirectory);
    std::filesystem::remove(directory);
#endif
}

void testVerifiedDestinationLeafSwapIsNotRetracted()
{
    const auto directory = temporaryDirectory();
    const auto destination = directory / "take.wav";
    std::string error;
    auto publication = talas::audio::AudioFilePublication::prepareContained(
        directory,
        "take.wav",
        talas::audio::ExistingAudioFilePolicy::Reject,
        error);
    require(publication != nullptr,
        "retraction leaf-swap publication should prepare: " + error);
    auto* output = publication->openTemporaryFile(error);
    require(output != nullptr,
        "retraction leaf-swap staging file should open: " + error);
    constexpr std::string_view verifiedBytes{"verified bytes"};
    require(
        std::fwrite(
            verifiedBytes.data(), 1, verifiedBytes.size(), output)
                == verifiedBytes.size()
            && std::fclose(output) == 0,
        "retraction fixture should persist verified bytes");
    require(publication->publish(error),
        "retraction fixture should publish: " + error);
    std::vector<std::uint8_t> snapshot;
    require(publication->readDestinationFile(1024U, snapshot, error)
            && std::string{snapshot.begin(), snapshot.end()} == verifiedBytes,
        "retraction fixture should bind the published inode");

    const auto verifiedLeaf = directory / "verified-leaf.wav";
#if defined(_WIN32)
    std::error_code renameError;
    std::filesystem::rename(destination, verifiedLeaf, renameError);
    require(renameError,
        "Windows retained destination handle must deny leaf replacement");
    require(publication->retract(error),
        "Windows exact publication should retract by handle: " + error);
    publication.reset();
    std::filesystem::remove(directory);
    return;
#else
    std::filesystem::rename(destination, verifiedLeaf);
    writeText(destination, "attacker bytes");
    require(!publication->retract(error),
        "retraction must reject a replaced destination leaf");
    require(readText(destination) == "attacker bytes",
        "rejected retraction must preserve the replacement file");
    require(readText(verifiedLeaf) == verifiedBytes,
        "rejected retraction must preserve the verified publication");

    std::filesystem::remove(destination);
    std::filesystem::rename(verifiedLeaf, destination);
    require(publication->retract(error),
        "restored exact publication should retract: " + error);
    publication.reset();
    std::filesystem::remove(directory);
#endif
}

void testVerifiedBytesRejectSameInodeMutation()
{
    const auto directory = temporaryDirectory();
    const auto destination = directory / "take.wav";
    std::string error;
    auto publication = talas::audio::AudioFilePublication::prepareContained(
        directory,
        "take.wav",
        talas::audio::ExistingAudioFilePolicy::Reject,
        error);
    require(publication != nullptr,
        "same-inode fixture should prepare: " + error);
    auto* output = publication->openTemporaryFile(error);
    require(output != nullptr,
        "same-inode fixture should open staging: " + error);
    require(std::fwrite("verified", 1, 8, output) == 8
            && std::fclose(output) == 0,
        "same-inode fixture should persist verified bytes");
    std::vector<std::uint8_t> snapshot;
    require(publication->readTemporaryFile(1024U, snapshot, error),
        "same-inode fixture should retain verified bytes");

#if defined(_WIN32)
    std::ofstream denied(
        publication->temporaryPath(),
        std::ios::binary | std::ios::trunc);
    require(!denied,
        "Windows retained file authority must deny a second writer");
    require(publication->publish(error),
        "Windows immutable held bytes should publish: " + error);
    publication.reset();
    std::filesystem::remove(destination);
#else
    writeText(publication->temporaryPath(), "mutated");
    require(publication->publish(error),
        "publication should install the retained verified bytes, not later same-inode mutations: "
            + error);
    require(readText(destination) == "verified",
        "same-inode staging mutation must not alter the published authority");
    const auto publishedPermissions =
        std::filesystem::status(destination).permissions();
    require(
        (publishedPermissions & std::filesystem::perms::all)
            == std::filesystem::perms::owner_read,
        "verified POSIX publication must install exact owner-read-only permissions");
    std::filesystem::remove(destination);
#endif
    publication.reset();
    std::filesystem::remove_all(directory);
}

void testRetractionRejectsSameInodeMutation()
{
    const auto directory = temporaryDirectory();
    const auto destination = directory / "take.wav";
    std::string error;
    auto publication = talas::audio::AudioFilePublication::prepareContained(
        directory,
        "take.wav",
        talas::audio::ExistingAudioFilePolicy::Reject,
        error);
    require(publication != nullptr,
        "retraction mutation fixture should prepare: " + error);
    auto* output = publication->openTemporaryFile(error);
    require(output != nullptr,
        "retraction mutation fixture should open staging: " + error);
    require(std::fwrite("verified", 1, 8, output) == 8
            && std::fclose(output) == 0,
        "retraction mutation fixture should persist verified bytes");
    require(publication->publish(error),
        "retraction mutation fixture should publish: " + error);
    std::vector<std::uint8_t> snapshot;
    require(publication->readDestinationFile(1024U, snapshot, error),
        "retraction mutation fixture should verify destination bytes");

#if defined(_WIN32)
    std::ofstream denied(destination, std::ios::binary | std::ios::trunc);
    require(!denied,
        "Windows retained destination authority must deny in-place mutation");
    require(publication->retract(error),
        "Windows unchanged exact destination should retract: " + error);
#else
    std::filesystem::permissions(
        destination,
        std::filesystem::perms::owner_read
            | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace);
    writeText(destination, "mutated");
    require(!publication->retract(error),
        "same-inode mutation after rollback inspection must reject retraction");
    require(readText(destination) == "mutated",
        "rejected retraction must restore the modified destination intact");
    require(publication->readDestinationFile(1024U, snapshot, error),
        "cleanup should intentionally adopt the modified fixture bytes");
    require(publication->retract(error),
        "adopted modified fixture should retract for cleanup: " + error);
#endif
    publication.reset();
    std::filesystem::remove(directory);
}

void testStagingDirectoryReplacementIsPreserved()
{
    const auto directory = temporaryDirectory();
    std::string error;
    auto publication = talas::audio::AudioFilePublication::prepareContained(
        directory,
        "take.wav",
        talas::audio::ExistingAudioFilePolicy::Reject,
        error);
    require(publication != nullptr,
        "staging replacement fixture should prepare: " + error);
    const auto staging = publication->temporaryPath().parent_path();
    const auto displaced = directory / "displaced-staging";
#if defined(_WIN32)
    std::error_code renameError;
    std::filesystem::rename(staging, displaced, renameError);
    require(renameError,
        "Windows held staging directory must deny replacement");
    require(publication->discard(error),
        "Windows held staging should discard exactly: " + error);
#else
    std::filesystem::rename(staging, displaced);
    require(std::filesystem::create_directory(staging),
        "staging replacement fixture should install an empty replacement");
    require(!publication->discard(error),
        "discard must reject a replaced staging directory");
    require(std::filesystem::exists(staging),
        "discard must preserve the unowned replacement directory");
    publication.reset();
    std::filesystem::remove(staging);
    std::filesystem::remove(displaced);
#endif
    publication.reset();
    std::filesystem::remove(directory);
}
}

// Private names must not grow with the destination name, or a long but
// valid file name exceeds the 255-byte component limit.
void testLongDestinationNamePublishesAndRetracts()
{
    const auto directory = temporaryDirectory();
    const auto destination = directory / (std::string(240, 'n') + ".wav");
    std::string error;
    auto publication = talas::audio::AudioFilePublication::prepare(
        destination,
        talas::audio::ExistingAudioFilePolicy::Reject,
        error);
    require(publication != nullptr,
        "a 244-byte destination name should prepare: " + error);
    writeText(publication->temporaryPath(), "long");
    require(publication->publish(error) && readText(destination) == "long",
        "a 244-byte destination name should publish: " + error);
    require(publication->retract(error)
            && !std::filesystem::exists(destination),
        "a 244-byte destination name should retract: " + error);
    std::filesystem::remove_all(directory);
}

// An exFAT image stands in for a USB stick or SD card, where macOS supports
// neither RENAME_EXCL nor RENAME_SWAP.
void testPublishesOnVolumesWithoutAtomicRenameFlags()
{
#if defined(__APPLE__)
    const auto directory = temporaryDirectory();
    const auto image = directory / "volume.dmg";
    const auto mount = directory / "mnt";
    const auto run = [](const std::string& command) {
        return std::system((command + " >/dev/null 2>&1").c_str()) == 0;
    };
    if (!run("hdiutil create -size 16m -fs ExFAT -volname KCPUB '"
            + image.string() + "'")
        || !run("hdiutil attach -nobrowse -mountpoint '" + mount.string()
            + "' '" + image.string() + "'"))
    {
        std::cout << "SKIP exFAT publication: hdiutil is unavailable\n";
        std::filesystem::remove_all(directory);
        return;
    }
    // A failed requirement terminates the test without unwinding, so the
    // image is detached explicitly before any exception leaves.
    const auto detach = [&] {
        (void) run("hdiutil detach -force '" + mount.string() + "'");
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    };
    try
    {
        const auto destination = mount / "take.wav";
        std::string error;
        auto created = talas::audio::AudioFilePublication::prepare(
            destination, talas::audio::ExistingAudioFilePolicy::Reject, error);
        require(created != nullptr, "exFAT destination should prepare: " + error);
        writeText(created->temporaryPath(), "first");
        require(created->publish(error) && readText(destination) == "first",
            "a new exFAT file should publish without RENAME_EXCL: " + error);

        auto replacement = talas::audio::AudioFilePublication::prepare(
            destination, talas::audio::ExistingAudioFilePolicy::Replace, error);
        require(replacement != nullptr,
            "exFAT replacement should prepare: " + error);
        writeText(replacement->temporaryPath(), "second");
        require(replacement->publish(error) && readText(destination) == "second",
            "an exFAT file should be replaced without RENAME_SWAP: " + error);
        require(replacement->retract(error)
                && !std::filesystem::exists(destination),
            "an exFAT publication should retract: " + error);
        for (const auto& entry : std::filesystem::directory_iterator(mount))
        {
            const auto name = entry.path().filename().string();
            require(name.find(".talas-") == std::string::npos,
                "no private staging entry may remain on the volume: " + name);
        }
    }
    catch (...)
    {
        detach();
        throw;
    }
    detach();
#endif
}

int main()
{
    testPublishesNewFileFromPrivateStaging();
    testExistingFileRequiresExplicitReplacement();
    testCancelRemovesOnlyPrivateTemporaryFile();
    testDestructorRemovesUnpublishedStaging();
    testDestinationRaceAndSymlinkFailClosed();
    testProtectedDestinationRelationships();
    testAncestorSwapCannotRedirectHeldPublication();
    testVerifiedStagingLeafSwapIsRejected();
    testVerifiedDestinationLeafSwapIsNotRetracted();
    testVerifiedBytesRejectSameInodeMutation();
    testRetractionRejectsSameInodeMutation();
    testStagingDirectoryReplacementIsPreserved();
    testLongDestinationNamePublishesAndRetracts();
    testPublishesOnVolumesWithoutAtomicRenameFlags();
    return 0;
}
