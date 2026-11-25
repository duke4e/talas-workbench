#include "talas/audio/SampleMapImport.h"
#include "talas/audio/WavWriter.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectAssetInspection.h"
#include "talas/project/ProjectEditor.h"
#include "talas/project/ProjectFile.h"
#include "talas/project/ProjectPackage.h"
#include "talas/project/ProjectPathEncoding.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
              / ("talas-sample-map-import-tests-"
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

    [[nodiscard]] const std::filesystem::path& path() const
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
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    require(output.good(), "text fixture should be writable");
}

void writeMono(
    const std::filesystem::path& path,
    double value,
    std::size_t frameCount = 64)
{
    std::filesystem::create_directories(path.parent_path());
    const std::vector<std::vector<double>> channels{
        std::vector<double>(frameCount, value),
    };
    std::string error;
    require(
        talas::audio::writePcm16Wav(path, 48000.0, channels, error),
        "mono WAV fixture should be writable: " + error);
}

void writeStereo(
    const std::filesystem::path& path,
    double left,
    double right,
    std::size_t frameCount = 96)
{
    std::filesystem::create_directories(path.parent_path());
    const std::vector<std::vector<double>> channels{
        std::vector<double>(frameCount, left),
        std::vector<double>(frameCount, right),
    };
    std::string error;
    require(
        talas::audio::writePcm16Wav(path, 44100.0, channels, error),
        "stereo WAV fixture should be writable: " + error);
}

talas::project::ProjectAsset inspectedAudio(
    const std::filesystem::path& path,
    std::string id)
{
    talas::project::ProjectAsset manifest;
    manifest.id = std::move(id);
    manifest.kind = talas::project::ProjectAssetKind::Audio;
    manifest.path = talas::project::pathToUtf8(path);
    const auto inspected = talas::project::inspectProjectAsset(
        manifest,
        path);
    require(inspected.ok(), "audio fixture should inspect");
    return inspected.asset;
}

std::filesystem::path createPackage(
    const std::filesystem::path& root)
{
    const auto package = root / "Import.talaspkg";
    std::filesystem::create_directories(
        package / talas::project::projectPackageAssetsDirectoryName);
    writeText(
        package / talas::project::projectPackageManifestFileName,
        talas::project::projectPackageManifestContents);
    require(
        talas::project::saveProjectFile(
            package / talas::project::projectPackageDocumentFileName,
            {})
            .ok(),
        "package fixture should save its project document");
    return package;
}

std::size_t regularFileCount(
    const std::filesystem::path& directory)
{
    std::size_t count = 0;
    std::error_code error;
    for (const auto& entry :
         std::filesystem::directory_iterator(directory, error))
    {
        if (entry.is_regular_file())
            ++count;
    }
    return count;
}

talas::core::NodeRegistry& builtinRegistry()
{
    static talas::core::NodeRegistry registry = [] {
        talas::core::NodeRegistry builtins;
        talas::core::registerBuiltinNodeTypes(builtins);
        return builtins;
    }();
    return registry;
}

void testReferenceImportIsDeterministicAndInspected()
{
    TemporaryDirectory directory;
    const auto sourceRoot = directory.path() / "sources";
    const auto snare = sourceRoot / "Z" / "Snare.wav";
    const auto kickB = sourceRoot / "B" / "KICK.wav";
    const auto kickA = sourceRoot / "A" / "kick.wav";
    writeMono(snare, 0.3);
    writeStereo(kickB, 0.1, -0.1);
    writeMono(kickA, -0.2);

    talas::audio::SampleMapImportOptions options;
    options.sourcePaths = {snare, kickB, kickA};
    options.projectFilePath = directory.path() / "Project.talas.json";
    options.disposition =
        talas::audio::SampleMapImportDisposition::Reference;
    options.lowNote = 36;
    options.highNote = 84;
    options.anchorRootNote = 60;

    double lastProgress = -1.0;
    options.reportProgress = [&](double progress) {
        require(
            progress >= lastProgress
                && progress >= 0.0
                && progress <= 1.0,
            "import progress should be monotonic and bounded");
        lastProgress = progress;
    };
    const auto imported = talas::audio::prepareSampleMapImport(
        {},
        options);
    require(
        imported.ok(),
        "reference import should succeed: "
            + (imported.error.has_value()
                   ? imported.error->message
                   : std::string{}));
    require(lastProgress == 1.0, "successful import should complete progress");
    require(
        imported.candidate.assets.size() == 3
            && imported.candidate.regions.size() == 3
            && imported.candidate.createdFiles.empty(),
        "reference import should inspect every source without copying bytes");
    require(
        imported.candidate.regions[0].regionId == "sample_kick"
            && imported.candidate.regions[1].regionId == "sample_kick_2"
            && imported.candidate.regions[2].regionId == "sample_snare",
        "candidate order and collision suffixes should be deterministic");
    require(
        imported.candidate.regions[0].rootNote == 60
            && imported.candidate.regions[1].rootNote == 61
            && imported.candidate.regions[2].rootNote == 62,
        "reference import should carry the deterministic auto-map preview");
    require(
        imported.candidate.autoMapCandidates.size() == 3
            && imported.candidate.autoMapCandidates[0].regionId
                == "sample_kick"
            && imported.candidate.autoMapCandidates[0].assetId
                == "Samples/kick"
            && imported.candidate.autoMapCandidates[0].sourceBasename
                == "kick.wav"
            && imported.candidate.autoMapCandidates[1].regionId
                == "sample_kick_2"
            && imported.candidate.autoMapCandidates[2].regionId
                == "sample_snare",
        "inspected import should retain the exact deterministic source candidates for preview-before-Apply");
    talas::project::ProjectSampleMapAutoMapRequest remapped;
    remapped.lowNote = 48;
    remapped.highNote = 72;
    remapped.anchorRootNote = 60;
    remapped.candidates = imported.candidate.autoMapCandidates;
    const auto alternatePreview =
        talas::project::makeAutoMappedSampleRegions(remapped);
    require(
        alternatePreview.ok()
            && alternatePreview.regions.size() == 3
            && alternatePreview.regions[0].lowNote == 48
            && alternatePreview.regions[0].rootNote == 60
            && alternatePreview.regions[2].highNote == 72,
        "retained candidates should deterministically recompute the user's preview range without reinspection");
    require(
        imported.candidate.assets[0].id == "Samples/kick"
            && imported.candidate.assets[1].id == "Samples/kick#2"
            && imported.candidate.assets[2].id == "Samples/snare",
        "project-local asset identities should use stable collision suffixes");
    require(
        imported.candidate.assets[0].kind
                == talas::project::ProjectAssetKind::Audio
            && imported.candidate.assets[0].disposition
                == talas::project::ProjectAssetDisposition::Referenced
            && imported.candidate.assets[0].contentHash.size() == 64
            && imported.candidate.assets[0].channelCount == 1
            && imported.candidate.assets[1].channelCount == 2
            && imported.candidate.assets[1].sampleRate == 44100.0
            && imported.candidate.assets[1].frameCount == 96
            && imported.candidate.assets[1].encoding == "pcm16-wav",
        "import should publish complete inspected PCM16 media records");
}

void testExactBytesReuseExistingIdentityAndBatchIdentity()
{
    TemporaryDirectory directory;
    const auto original = directory.path() / "original.wav";
    const auto duplicate = directory.path() / "duplicate.wav";
    writeMono(original, 0.25);
    std::filesystem::copy_file(original, duplicate);

    talas::project::ProjectDocument document;
    document.assets.push_back(
        inspectedAudio(original, "Existing/Exact"));
    talas::audio::SampleMapImportOptions options;
    options.sourcePaths = {duplicate};
    const auto reused = talas::audio::prepareSampleMapImport(
        document,
        options);
    require(reused.ok(), "exact existing bytes should import");
    require(
        reused.candidate.assets.empty()
            && reused.candidate.regions.size() == 1
            && reused.candidate.regions.front().assetId
                == "Existing/Exact",
        "exact existing bytes should reuse manifest identity without a duplicate asset");

    talas::audio::SampleMapImportOptions duplicates;
    duplicates.sourcePaths = {original, duplicate};
    const auto withinBatch = talas::audio::prepareSampleMapImport(
        {},
        duplicates);
    require(withinBatch.ok(), "same bytes twice should build one batch asset");
    require(
        withinBatch.candidate.assets.size() == 1
            && withinBatch.candidate.regions.size() == 2
            && withinBatch.candidate.regions[0].assetId
                == withinBatch.candidate.regions[1].assetId,
        "same bytes in one batch should reuse its first deterministic identity");
}

void testExactBytesIgnoreAssetsOfAnotherKind()
{
    TemporaryDirectory directory;
    const auto original = directory.path() / "original.wav";
    const auto duplicate = directory.path() / "duplicate.wav";
    writeMono(original, 0.25);
    std::filesystem::copy_file(original, duplicate);

    auto table = inspectedAudio(original, "Tables/SameBytes");
    table.kind = talas::project::ProjectAssetKind::Table;
    table.channelCount = 0;
    table.sampleRate = 0.0;
    table.frameCount = 0;
    table.encoding.clear();
    talas::project::ProjectDocument document;
    document.assets.push_back(table);
    talas::audio::SampleMapImportOptions options;
    options.sourcePaths = {duplicate};
    const auto imported = talas::audio::prepareSampleMapImport(
        document,
        options);
    require(imported.ok()
            && imported.candidate.assets.size() == 1
            && imported.candidate.assets.front().kind
                == talas::project::ProjectAssetKind::Audio
            && imported.candidate.regions.size() == 1
            && imported.candidate.regions.front().assetId
                == imported.candidate.assets.front().id,
        "a Table holding the same bytes must not be taken for the Audio asset: "
            + (imported.error.has_value() ? imported.error->message
                                           : std::string{}));
}

void testUninspectedSamePathIdentityIsCompletedAtomically()
{
    TemporaryDirectory directory;
    const auto source = directory.path() / "pending.wav";
    writeMono(source, 0.125);

    talas::project::ProjectDocument document;
    talas::project::ProjectAsset pending;
    pending.id = "Existing/Pending";
    pending.kind = talas::project::ProjectAssetKind::Audio;
    pending.path = talas::project::pathToUtf8(source);
    document.assets.push_back(pending);
    talas::project::ProjectSampleMap map;
    map.mapId = "mainMap";
    map.displayName = "Main";
    document.sampleMapLibrary.maps.push_back(std::move(map));

    talas::audio::SampleMapImportOptions options;
    options.sourcePaths = {source};
    const auto prepared = talas::audio::prepareSampleMapImport(
        document,
        options);
    require(
        prepared.ok()
            && prepared.candidate.assets.size() == 1
            && prepared.candidate.assets.front().id == pending.id
            && !prepared.candidate.assets.front().contentHash.empty()
            && prepared.candidate.regions.front().assetId == pending.id,
        "same-path import should complete the existing identity instead of creating a duplicate");

    talas::project::ProjectEditor editor(document, builtinRegistry());
    talas::project::ProjectSampleMapImportBatch batch;
    batch.assets = prepared.candidate.assets;
    batch.expectedAssets = prepared.candidate.expectedAssets;
    batch.regions = prepared.candidate.regions;
    require(
        editor.mergeSampleMapImport("mainMap", std::move(batch)).ok(),
        "same-path inspection and region publication should commit atomically");
    require(
        editor.document().assets.size() == 1
            && editor.document().assets.front().id == pending.id
            && !editor.document().assets.front().contentHash.empty()
            && editor.document().sampleMapLibrary.maps.front().regions.front()
                    .assetId
                == pending.id,
        "atomic publication should preserve one completed manifest identity");
    require(
        editor.undo().ok()
            && editor.document().assets == document.assets
            && editor.document().sampleMapLibrary
                == document.sampleMapLibrary,
        "one undo should restore both pending manifest and empty map");
}

void testCancellationAndInvalidMediaPublishNothing()
{
    TemporaryDirectory directory;
    const auto first = directory.path() / "first.wav";
    const auto second = directory.path() / "second.wav";
    writeMono(first, 0.1, 8192);
    writeMono(second, 0.2, 8192);

    talas::audio::SampleMapImportOptions cancelled;
    cancelled.sourcePaths = {first, second};
    cancelled.shouldCancel = [] { return true; };
    const auto cancelledResult =
        talas::audio::prepareSampleMapImport({}, cancelled);
    require(
        !cancelledResult.ok()
            && cancelledResult.cancelled()
            && cancelledResult.candidate.assets.empty()
            && cancelledResult.candidate.regions.empty()
            && cancelledResult.candidate.createdFiles.empty(),
        "cancelled import should publish no inert candidate or copied bytes");

    const auto invalid = directory.path() / "not-a-wave.wav";
    writeText(invalid, "not audio");
    talas::audio::SampleMapImportOptions invalidOptions;
    invalidOptions.sourcePaths = {first, invalid};
    const auto invalidResult =
        talas::audio::prepareSampleMapImport({}, invalidOptions);
    require(
        !invalidResult.ok()
            && invalidResult.error->code
                == talas::audio::SampleMapImportErrorCode::InspectionFailed
            && invalidResult.candidate.assets.empty()
            && invalidResult.candidate.regions.empty(),
        "one invalid source should reject the complete multi-file candidate");
}

void testInspectedBatchCanDeferAnInitiallyInvalidAutoMapRange()
{
    TemporaryDirectory directory;
    talas::audio::SampleMapImportOptions options;
    options.deferAutoMapRangeValidation = true;
    for (std::size_t index = 0; index < 69; ++index)
    {
        const auto source = directory.path()
            / ("sample_" + std::to_string(index) + ".wav");
        writeMono(source, 0.1);
        options.sourcePaths.push_back(source);
    }

    const auto imported = talas::audio::prepareSampleMapImport({}, options);
    require(
        imported.ok()
            && imported.candidate.autoMapCandidates.size() == 69
            && imported.candidate.regions.empty(),
        "inspection should retain a large inert batch for user range selection before Apply");

    talas::project::ProjectSampleMapAutoMapRequest adjusted;
    adjusted.lowNote = 0;
    adjusted.anchorRootNote = 0;
    adjusted.highNote = 127;
    adjusted.candidates = imported.candidate.autoMapCandidates;
    const auto preview = talas::project::makeAutoMappedSampleRegions(adjusted);
    require(
        preview.ok() && preview.regions.size() == 69,
        "a user-adjusted valid range should map the inspected batch without reinspection");
}

void testEmbeddedImportCopiesVerifiesAndCanBeDiscarded()
{
    TemporaryDirectory directory;
    const auto package = createPackage(directory.path());
    const auto source = directory.path() / "outside" / "voice.wav";
    writeStereo(source, 0.4, -0.4);

    talas::audio::SampleMapImportOptions options;
    options.sourcePaths = {source};
    options.projectFilePath = package;
    options.disposition =
        talas::audio::SampleMapImportDisposition::Embed;
    auto imported = talas::audio::prepareSampleMapImport({}, options);
    require(
        imported.ok()
            && imported.candidate.assets.size() == 1
            && imported.candidate.createdFiles.size() == 1,
        "embedded import should copy one fully inspected source");
    const auto& asset = imported.candidate.assets.front();
    require(
        asset.disposition
                == talas::project::ProjectAssetDisposition::Embedded
            && asset.path.starts_with("Assets/")
            && std::filesystem::is_regular_file(
                imported.candidate.createdFiles.front()),
        "embedded candidate should point at a verified managed copy");

    const auto cleanup =
        talas::audio::discardPreparedSampleMapImportFiles(
            imported.candidate);
    require(cleanup.empty(), "discarding uncommitted copies should succeed");
    require(
        imported.candidate.createdFiles.empty()
            && regularFileCount(
                   package
                   / talas::project::projectPackageAssetsDirectoryName)
                == 0,
        "discard should remove only candidate-created embedded files");
}

void testEmbeddedImportAdoptsReferencedSamePathAndExactHashAssets()
{
    TemporaryDirectory directory;
    const auto package = createPackage(directory.path());
    const auto original = directory.path() / "outside" / "voice.wav";
    const auto alias = directory.path() / "alias" / "same-voice.wav";
    writeMono(original, 0.375);
    std::filesystem::create_directories(alias.parent_path());
    std::filesystem::copy_file(original, alias);

    talas::project::ProjectDocument document;
    document.assets.push_back(
        inspectedAudio(original, "Existing/Voice"));
    talas::project::ProjectSampleMap map;
    map.mapId = "mainMap";
    map.displayName = "Main";
    document.sampleMapLibrary.maps.push_back(std::move(map));

    const auto prepare = [&](const std::filesystem::path& source) {
        talas::audio::SampleMapImportOptions options;
        options.sourcePaths = {source};
        options.projectFilePath = package;
        options.disposition =
            talas::audio::SampleMapImportDisposition::Embed;
        return talas::audio::prepareSampleMapImport(document, options);
    };

    auto samePath = prepare(original);
    require(
        samePath.ok()
            && samePath.candidate.assets.size() == 1
            && samePath.candidate.expectedAssets
                == document.assets
            && samePath.candidate.assets.front().id == "Existing/Voice"
            && samePath.candidate.assets.front().disposition
                == talas::project::ProjectAssetDisposition::Embedded
            && samePath.candidate.createdFiles.size() == 1
            && samePath.candidate.regions.front().assetId
                == "Existing/Voice",
        "Embed should adopt a same-path referenced identity into one managed copy candidate");

    talas::project::ProjectEditor staleEditor(document, builtinRegistry());
    auto relocated = document.assets.front();
    relocated.path = talas::project::pathToUtf8(alias);
    require(
        staleEditor.acceptProjectAssetDisposition(
            relocated,
            document.assets.front()).ok(),
        "stale fixture should publish a newer same-hash relocation");
    talas::project::ProjectSampleMapImportBatch staleBatch;
    staleBatch.assets = samePath.candidate.assets;
    staleBatch.expectedAssets = samePath.candidate.expectedAssets;
    staleBatch.regions = samePath.candidate.regions;
    const auto stale = staleEditor.mergeSampleMapImport(
        "mainMap",
        std::move(staleBatch));
    require(
        !stale.ok()
            && stale.error
                == talas::project::EditError::StaleAssetDisposition
            && staleEditor.document().assets.front() == relocated
            && staleEditor.document().sampleMapLibrary.maps.front()
                .regions.empty(),
        "Embed reuse should reject a candidate prepared before a concurrent relocation");
    require(
        talas::audio::discardPreparedSampleMapImportFiles(
            samePath.candidate).empty()
            && samePath.candidate.createdFiles.empty()
            && staleEditor.undo().ok()
            && staleEditor.document().assets == document.assets
            && staleEditor.document().sampleMapLibrary
                == document.sampleMapLibrary
            && !staleEditor.undo().ok(),
        "stale publication cleanup should remove its managed copy and add no Undo state");

    auto exactHash = prepare(alias);
    require(
        exactHash.ok()
            && exactHash.candidate.assets.size() == 1
            && exactHash.candidate.expectedAssets
                == document.assets
            && exactHash.candidate.assets.front().id == "Existing/Voice"
            && exactHash.candidate.assets.front().disposition
                == talas::project::ProjectAssetDisposition::Embedded
            && exactHash.candidate.createdFiles.size() == 1
            && exactHash.candidate.regions.front().assetId
                == "Existing/Voice",
        "Embed should adopt an exact-hash referenced identity instead of silently retaining Reference");
    require(
        talas::audio::discardPreparedSampleMapImportFiles(
            exactHash.candidate).empty()
            && regularFileCount(
                   package
                   / talas::project::projectPackageAssetsDirectoryName)
                == 0,
        "discard should remove the exact-hash managed copy without touching source bytes");
}

void testEmbeddedCopyFailureRollsBackEarlierCopies()
{
    TemporaryDirectory directory;
    const auto package = createPackage(directory.path());
    const auto first = directory.path() / "sources" / "First.wav";
    const auto second = directory.path() / "sources" / "Second.wav";
    writeMono(first, 0.1);
    writeMono(second, 0.2);

    talas::audio::SampleMapImportOptions options;
    options.sourcePaths = {first, second};
    options.projectFilePath = package;
    options.disposition =
        talas::audio::SampleMapImportDisposition::Embed;
    auto removedSecond = false;
    options.reportProgress = [&](double progress) {
        if (!removedSecond && progress >= 0.75 && progress < 1.0)
        {
            std::error_code error;
            std::filesystem::remove(second, error);
            removedSecond = true;
        }
    };

    const auto imported = talas::audio::prepareSampleMapImport({}, options);
    require(
        !imported.ok()
            && imported.error->code
                == talas::audio::SampleMapImportErrorCode::CopyFailed,
        "second embedded copy failure should reject the complete import");
    require(
        imported.candidate.assets.empty()
            && imported.candidate.regions.empty()
            && imported.candidate.createdFiles.empty()
            && regularFileCount(
                   package
                   / talas::project::projectPackageAssetsDirectoryName)
                == 0,
        "copy failure should remove every earlier candidate-created file");
}
}

int main()
{
    testReferenceImportIsDeterministicAndInspected();
    testExactBytesReuseExistingIdentityAndBatchIdentity();
    testExactBytesIgnoreAssetsOfAnotherKind();
    testUninspectedSamePathIdentityIsCompletedAtomically();
    testCancellationAndInvalidMediaPublishNothing();
    testInspectedBatchCanDeferAnInitiallyInvalidAutoMapRange();
    testEmbeddedImportCopiesVerifiesAndCanBeDiscarded();
    testEmbeddedImportAdoptsReferencedSamePathAndExactHashAssets();
    testEmbeddedCopyFailureRollsBackEarlierCopies();
    return 0;
}
