#include "talas/audio/ProjectAssetChangeTracker.h"
#include "talas/core/BuiltinNodes.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

const talas::core::NodeRegistry& registry()
{
    static const auto value = talas::core::makeBuiltinNodeRegistry();
    return value;
}

class TemporaryDirectory
{
public:
    TemporaryDirectory()
        : path_(
            std::filesystem::temp_directory_path()
            / ("talas-project-asset-change-tests-"
               + std::to_string(
                   std::chrono::steady_clock::now()
                       .time_since_epoch()
                       .count())))
    {
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path()
        const
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void writeText(
    const std::filesystem::path& path,
    std::string_view text)
{
    std::filesystem::create_directories(
        path.parent_path());
    std::ofstream output(
        path,
        std::ios::binary | std::ios::trunc);
    output << text;
    require(
        output.good(),
        "test fixture should write text");
}

talas::project::ProjectParameterExpression resource(
    std::string id,
    std::string assetId)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.resource = std::move(assetId);
    return parameter;
}

talas::project::ProjectDocument watchedDocument()
{
    talas::project::ProjectDocument document;
    document.assets = {
        {"Used",
         talas::project::ProjectAssetKind::Table,
         "assets/used.txt"},
        {"Unused",
         talas::project::ProjectAssetKind::Binary,
         "assets/unused.bin"},
    };
    document.graph.nodes.push_back({
        "sample",
        "talas.sample",
        {resource("sample", "Used")},
        {},
    });
    return document;
}

void testStableSnapshotsResolveOnlyReferencedAssets()
{
    TemporaryDirectory directory;
    writeText(
        directory.path() / "assets" / "used.txt",
        "1 0 -1");
    auto document = watchedDocument();
    const auto project =
        directory.path() / "Sound.talas.json";

    const auto first =
        talas::audio::
            captureProjectAssetWatchSnapshot(
                document,
                registry(),
                project);
    const auto second =
        talas::audio::
            captureProjectAssetWatchSnapshot(
                document,
                registry(),
                project);
    require(
        first.entries.size() == 1
            && first.entries.front().assetId == "Used"
            && first.entries.front().resolvedPath
                == directory.path()
                    / "assets"
                    / "used.txt",
        "watch snapshots should resolve only graph-referenced assets");
    require(
        !talas::audio::findProjectAssetWatchChange(
             first,
             second)
             .has_value(),
        "unchanged asset snapshots should compare equal");
}

void testModificationDeletionAndCreationAreDetected()
{
    TemporaryDirectory directory;
    const auto asset =
        directory.path() / "assets" / "used.txt";
    writeText(asset, "1");
    const auto project =
        directory.path() / "Sound.talas.json";
    const auto document = watchedDocument();
    const auto initial =
        talas::audio::
            captureProjectAssetWatchSnapshot(
                document,
                registry(),
                project);

    writeText(asset, "1 0 -1");
    const auto modified =
        talas::audio::
            captureProjectAssetWatchSnapshot(
                document,
                registry(),
                project);
    const auto modification =
        talas::audio::findProjectAssetWatchChange(
            initial,
            modified);
    require(
        modification.has_value()
            && modification->assetId == "Used",
        "asset content modification should be detected");

    std::filesystem::remove(asset);
    const auto missing =
        talas::audio::
            captureProjectAssetWatchSnapshot(
                document,
                registry(),
                project);
    require(
        talas::audio::findProjectAssetWatchChange(
            modified,
            missing)
            .has_value(),
        "asset deletion should be detected");

    writeText(asset, "restored");
    const auto restored =
        talas::audio::
            captureProjectAssetWatchSnapshot(
                document,
                registry(),
                project);
    require(
        talas::audio::findProjectAssetWatchChange(
            missing,
            restored)
            .has_value(),
        "asset creation should be detected");
}

void testAcceptedGenerationChangesAreDetectedWithoutPathChanges()
{
    TemporaryDirectory directory;
    writeText(
        directory.path() / "assets" / "used.txt",
        "1 0 -1");
    auto document = watchedDocument();
    document.assets.front().contentHash.assign(64, 'a');
    const auto project =
        directory.path() / "Sound.talas.json";
    const auto before =
        talas::audio::captureProjectAssetWatchSnapshot(
            document,
            registry(),
            project);

    document.assets.front().generation = 2;
    document.assets.front().contentHash.assign(64, 'b');
    const auto after =
        talas::audio::captureProjectAssetWatchSnapshot(
            document,
            registry(),
            project);
    const auto change =
        talas::audio::findProjectAssetWatchChange(
            before,
            after);
    require(
        change.has_value()
            && change->assetId == "Used"
            && change->message
                == "Accepted asset generation changed",
        "watch authority should detect accepted generation changes even when path and file metadata are unchanged");
}

void testSymlinkRetargetingIsDetected()
{
    TemporaryDirectory directory;
    const auto firstTarget =
        directory.path() / "one.txt";
    const auto secondTarget =
        directory.path() / "two.txt";
    writeText(firstTarget, "same");
    writeText(secondTarget, "same");
    const auto link =
        directory.path() / "assets" / "used.txt";
    std::filesystem::create_directories(
        link.parent_path());
    std::filesystem::create_symlink(
        firstTarget,
        link);

    const auto document = watchedDocument();
    const auto project =
        directory.path() / "Sound.talas.json";
    const auto before =
        talas::audio::
            captureProjectAssetWatchSnapshot(
                document,
                registry(),
                project);
    std::filesystem::remove(link);
    std::filesystem::create_symlink(
        secondTarget,
        link);
    const auto after =
        talas::audio::
            captureProjectAssetWatchSnapshot(
                document,
                registry(),
                project);
    require(
        talas::audio::findProjectAssetWatchChange(
            before,
            after)
            .has_value(),
        "symlink target changes should be detected even for equal-size files");
}

void testReachableSoundDefinitionAssetsAreWatched()
{
    TemporaryDirectory directory;
    writeText(
        directory.path() / "assets" / "used.txt",
        "1 0 -1");
    auto document = watchedDocument();
    document.graph.nodes.clear();
    talas::project::ProjectSoundDefinition voice;
    voice.definitionId = "sampleVoice";
    voice.displayName = "Sample Voice";
    voice.outputs.push_back({
        "audioOut",
        talas::core::PortType::Audio,
        "sample",
        "audioOut",
    });
    voice.graph.nodes.push_back({
        "sample",
        "talas.sample",
        {resource("sample", "Used")},
        {},
    });
    document.soundDefinitions.push_back(std::move(voice));
    talas::project::ProjectNode instance;
    instance.instanceId = "voice1";
    instance.soundDefinitionId = "sampleVoice";
    document.graph.nodes.push_back(std::move(instance));
    document.graph.nodes.push_back({
        "out",
        "talas.audioOutput",
        {},
        {},
    });
    document.graph.connections.push_back({
        "voice1",
        "audioOut",
        "out",
        "audioIn",
    });

    const auto snapshot =
        talas::audio::captureProjectAssetWatchSnapshot(
            document,
            registry(),
            directory.path() / "Sound.talas.json");
    require(
        snapshot.entries.size() == 1
            && snapshot.entries.front().assetId == "Used",
        "reachable reusable Sound resources should participate in the external-change baseline");
}
}

int main()
{
    testStableSnapshotsResolveOnlyReferencedAssets();
    testModificationDeletionAndCreationAreDetected();
    testAcceptedGenerationChangesAreDetectedWithoutPathChanges();
    testSymlinkRetargetingIsDetected();
    testReachableSoundDefinitionAssetsAreWatched();
    return 0;
}
