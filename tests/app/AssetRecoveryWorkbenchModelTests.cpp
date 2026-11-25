#include "talas/app/AssetRecoveryWorkbenchModel.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
class TemporaryDirectory
{
public:
    TemporaryDirectory()
        : path_(std::filesystem::temp_directory_path()
                / ("talas-asset-recovery-inventory-"
                   + std::to_string(
                       std::chrono::steady_clock::now()
                           .time_since_epoch()
                           .count())))
    {
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void writeBytes(
    const std::filesystem::path& path,
    std::string_view bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!output.good())
        throw std::runtime_error("unable to write inventory fixture");
}

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

talas::project::ProjectAsset asset(
    std::string id,
    bool verified = true)
{
    talas::project::ProjectAsset value;
    value.id = std::move(id);
    value.kind = talas::project::ProjectAssetKind::Binary;
    value.path = "missing/asset.bin";
    value.generation = 7;
    if (verified)
        value.contentHash.assign(64, 'a');
    return value;
}

talas::project::ProjectAssetResolution resolution(
    const talas::project::ProjectAsset& manifest,
    talas::project::ProjectAssetResolutionMode mode,
    std::string path,
    char hash = 'a')
{
    auto candidate = manifest;
    candidate.path = std::move(path);
    candidate.contentHash.assign(64, hash);
    return {
        mode,
        manifest.generation,
        manifest.contentHash,
        std::move(candidate),
    };
}

void testInventorySurfacesEveryChangedRegularAsset()
{
    TemporaryDirectory directory;
    const auto projectPath = directory.path() / "Project.talas.json";
    const auto firstPath = directory.path() / "first.bin";
    const auto secondPath = directory.path() / "second.bin";
    const auto exactPath = directory.path() / "exact.bin";

    writeBytes(firstPath, "first accepted bytes");
    const auto firstAccepted =
        talas::project::calculateProjectAssetSha256(firstPath);
    writeBytes(firstPath, "first changed bytes");
    writeBytes(secondPath, "second accepted bytes");
    const auto secondAccepted =
        talas::project::calculateProjectAssetSha256(secondPath);
    writeBytes(secondPath, "second changed bytes");
    writeBytes(exactPath, "exact accepted bytes");
    const auto exactAccepted =
        talas::project::calculateProjectAssetSha256(exactPath);
    require(
        firstAccepted.ok() && secondAccepted.ok() && exactAccepted.ok(),
        "inventory fixtures should hash");

    auto first = asset("Samples/FirstChanged");
    first.path = "first.bin";
    first.contentHash = firstAccepted.contentHash;
    auto second = asset("Samples/SecondChanged");
    second.path = "second.bin";
    second.contentHash = secondAccepted.contentHash;
    auto exact = asset("Samples/ExactAvailable");
    exact.path = "exact.bin";
    exact.contentHash = exactAccepted.contentHash;
    const std::vector<talas::app::AssetRecoveryInventoryEntry> entries{
        {first, "root/first.sample"},
        {second, "root/second.sample"},
        {exact, "root/exact.sample"},
    };

    auto result = talas::app::inspectAssetRecoveryInventory(
        entries,
        projectPath);
    require(
        result.status
                == talas::app::AssetRecoveryInventoryStatus::Completed
            && result.items.size() == 2
            && result.items[0].manifest.id == first.id
            && result.items[1].manifest.id == second.id
            && std::ranges::all_of(
                result.items,
                [](const auto& item) {
                    return item.state
                            == talas::app::AssetRecoveryItemState::ReplacementCandidate
                        && item.candidates.size() == 1
                        && item.selectedCandidateIndex
                            == std::optional<std::size_t>{0}
                        && item.candidates.front().mode
                            == talas::project::ProjectAssetResolutionMode::Replacement;
                }),
        "one off-thread inventory should surface every changed regular asset and omit exact available assets");

    talas::app::AssetRecoveryWorkbenchCoordinator coordinator({{
        first,
        talas::app::AssetRecoveryItemState::Changed,
        "root/first.sample",
        "First load failure",
    }});
    const auto generation = coordinator.beginInventoryInspection();
    require(
        coordinator.finishInventoryInspection(
            generation,
            std::move(result))
            && coordinator.state().items.size() == 2,
        "the owned inventory generation should publish the complete batch atomically");

    auto missingOne = asset("Samples/MissingOne");
    missingOne.path = "missing-one.bin";
    auto missingTwo = asset("Samples/MissingTwo");
    missingTwo.path = "missing-two.bin";
    const std::vector<talas::app::AssetRecoveryInventoryEntry>
        missingEntries{
            {missingOne, "root/missing-one.sample"},
            {missingTwo, "root/missing-two.sample"},
        };
    int cancellationChecks = 0;
    const auto cancelled = talas::app::inspectAssetRecoveryInventory(
        missingEntries,
        projectPath,
        [&] { return ++cancellationChecks >= 2; });
    require(
        cancelled.status
                == talas::app::AssetRecoveryInventoryStatus::Cancelled
            && cancelled.items.empty(),
        "inventory cancellation after earlier rows must discard every partial result");
}

void testProjectionSeparatesResolutionAndReplacementActions()
{
    const auto missing = asset("Samples/Missing");
    const auto unverified = asset("Samples/Unverified", false);
    const auto exact = asset("Samples/Exact");
    const auto ambiguous = asset("Samples/Ambiguous");
    const auto changed = asset("Samples/Changed");
    const auto ignored = asset("Samples/Ignored");

    talas::app::AssetRecoveryWorkbenchState state;
    state.items = {
        {
            missing,
            talas::app::AssetRecoveryItemState::Missing,
            "root/sample.sample",
            "Asset is missing",
        },
        {
            unverified,
            talas::app::AssetRecoveryItemState::Unverified,
            "root/legacy.sample",
            "Choose a file for first inspection",
        },
        {
            exact,
            talas::app::AssetRecoveryItemState::ExactMatch,
            "root/exact.sample",
            {},
            {resolution(
                exact,
                talas::project::ProjectAssetResolutionMode::Relocation,
                "found/exact.bin")},
            0,
        },
        {
            ambiguous,
            talas::app::AssetRecoveryItemState::Ambiguous,
            "root/ambiguous.sample",
            {},
            {
                resolution(
                    ambiguous,
                    talas::project::ProjectAssetResolutionMode::Relocation,
                    "a/ambiguous.bin"),
                resolution(
                    ambiguous,
                    talas::project::ProjectAssetResolutionMode::Relocation,
                    "b/ambiguous.bin"),
            },
        },
        {
            changed,
            talas::app::AssetRecoveryItemState::ReplacementCandidate,
            "root/changed.sample",
            "Different content requires confirmation",
            {resolution(
                changed,
                talas::project::ProjectAssetResolutionMode::Replacement,
                "replacement/changed.bin",
                'b')},
            0,
        },
        {
            ignored,
            talas::app::AssetRecoveryItemState::Ignored,
            "root/ignored.sample",
            "Ignored for this session",
            {},
            std::nullopt,
            true,
        },
    };

    const auto projection = talas::app::assetRecoveryWorkbenchProjection(state);
    require(
        projection.rows.size() == 6
            && projection.canSearchFolder
            && projection.canApplyRelocations
            && !projection.canCancelSearch
            && projection.applyCount == 1
            && projection.unresolvedCount == 5,
        "projection should expose one exact apply while retaining every unresolved non-ignored row");
    require(
        projection.rows[0].canLocateFile
            && projection.rows[0].searchEligible
            && projection.rows[0].canIgnore,
        "verified missing asset should support locate, folder search, and ignore");
    require(
        projection.rows[1].canLocateFile
            && !projection.rows[1].searchEligible,
        "unverified assets should require explicit file choice instead of hash search");
    require(
        projection.rows[2].selectedForApply
            && !projection.rows[2].canReplace,
        "exact match should enter apply batch without replacement confirmation");
    require(
        projection.rows[3].canChooseCandidate
            && !projection.rows[3].selectedForApply,
        "ambiguous exact matches should require one explicit path selection");
    require(
        projection.rows[4].canReplace
            && !projection.rows[4].selectedForApply,
        "different content should expose Replace but never enter relocation apply");
    require(
        !projection.rows[5].canLocateFile
            && !projection.rows[5].canIgnore,
        "ignored row should remain visible without active actions");

    const auto batch = talas::app::assetRecoveryApplyBatch(state);
    require(
        batch.size() == 1
            && batch.front().asset.id == "Samples/Exact"
            && batch.front().mode
                == talas::project::ProjectAssetResolutionMode::Relocation,
        "apply batch should contain only selected exact/initial resolutions");
}

void testProgressAccessibilityAndCompactPolicyRemainStable()
{
    talas::app::AssetRecoveryWorkbenchState state;
    state.searchActive = true;
    state.progress = {42, 7, "Drums/Kick.wav"};
    state.items.push_back({
        asset("Samples/Kick"),
        talas::app::AssetRecoveryItemState::Searching,
        "voice/kick.sample",
        "Searching selected folder",
    });
    const auto projection = talas::app::assetRecoveryWorkbenchProjection(state);
    require(
        projection.canCancelSearch
            && !projection.canSearchFolder
            && !projection.canApplyRelocations
            && projection.statusText
                == "Searching assets · 7 files hashed · 42 entries visited · Drums/Kick.wav",
        "active search should expose bounded progress and only its cancel action");
    require(
        projection.rows.front().accessibleDescription
                .find("Samples/Kick") != std::string::npos
            && projection.rows.front().accessibleDescription
                .find("generation 7") != std::string::npos
            && projection.rows.front().accessibleDescription
                .find("voice/kick.sample") != std::string::npos,
        "row accessibility copy should include identity, generation, and structural source");
    require(
        talas::app::assetRecoveryLayoutMode(359)
                == talas::app::AssetRecoveryLayoutMode::Compact
            && talas::app::assetRecoveryLayoutMode(360)
                == talas::app::AssetRecoveryLayoutMode::Wide,
        "secondary controls should compact at one stable width boundary");
    require(
        talas::app::assetRecoveryKeyboardOrder()
            == std::vector<talas::app::AssetRecoveryFocusTarget>{
                talas::app::AssetRecoveryFocusTarget::Rows,
                talas::app::AssetRecoveryFocusTarget::LocateFile,
                talas::app::AssetRecoveryFocusTarget::SearchFolder,
                talas::app::AssetRecoveryFocusTarget::ApplyRelocations,
                talas::app::AssetRecoveryFocusTarget::Replace,
                talas::app::AssetRecoveryFocusTarget::Ignore,
                talas::app::AssetRecoveryFocusTarget::CancelSearch,
            },
        "keyboard order should remain explicit and independent from visual compaction");
}

void testCoordinatorRejectsLateResultsAndKeepsReplacementExplicit()
{
    const auto first = asset("Samples/First");
    const auto second = asset("Samples/Second");
    talas::app::AssetRecoveryWorkbenchCoordinator coordinator({
        {
            first,
            talas::app::AssetRecoveryItemState::Missing,
            "root/first.sample",
            "Missing",
        },
        {
            second,
            talas::app::AssetRecoveryItemState::Missing,
            "root/second.sample",
            "Missing",
        },
    });

    const auto generation = coordinator.beginSearch();
    require(
        coordinator.publishSearchProgress(
            generation,
            {12, 4, "Samples/scan.bin"})
            && std::ranges::all_of(
                coordinator.state().items,
                [](const auto& item) {
                    return item.state
                        == talas::app::AssetRecoveryItemState::Searching;
                }),
        "current worker generation should publish coalesced progress");
    talas::project::ProjectAssetSearchResult result;
    result.matches = {
        {
            first.id,
            {resolution(
                first,
                talas::project::ProjectAssetResolutionMode::Relocation,
                "found/first.bin")},
        },
        {
            second.id,
            {
                resolution(
                    second,
                    talas::project::ProjectAssetResolutionMode::Relocation,
                    "a/second.bin"),
                resolution(
                    second,
                    talas::project::ProjectAssetResolutionMode::Relocation,
                    "b/second.bin"),
            },
        },
    };
    require(
        coordinator.finishSearch(generation, result),
        "current complete search should publish atomically");
    auto projection = talas::app::assetRecoveryWorkbenchProjection(
        coordinator.state());
    require(
        projection.applyCount == 1
            && projection.rows[1].canChooseCandidate,
        "one exact and one ambiguous result should remain distinct");
    require(
        coordinator.selectCandidate("Samples/Second", 1),
        "ambiguous path should become selectable explicitly");
    projection = talas::app::assetRecoveryWorkbenchProjection(
        coordinator.state());
    require(
        projection.applyCount == 2,
        "selected ambiguous exact match should join the relocation batch");

    const auto replacementCandidate = resolution(
        first,
        talas::project::ProjectAssetResolutionMode::Replacement,
        "replacement/first.bin",
        'b');
    require(
        coordinator.setCandidate("Samples/First", replacementCandidate)
            && coordinator.requestReplacementConfirmation("Samples/First"),
        "different bytes should require a named confirmation state");
    const auto confirmed = coordinator.confirmedReplacementCandidate();
    require(
        confirmed.has_value()
            && confirmed->mode
                == talas::project::ProjectAssetResolutionMode::Replacement,
        "confirmation should expose only the selected replacement candidate");

    const auto staleGeneration = coordinator.beginSearch();
    coordinator.replaceItems({{
        asset("Samples/NewProject"),
        talas::app::AssetRecoveryItemState::Missing,
        "new/root.sample",
        "Missing",
    }});
    require(
        !coordinator.publishSearchProgress(
            staleGeneration,
            {99, 99, "late.bin"})
            && !coordinator.finishSearch(staleGeneration, result)
            && coordinator.state().items.size() == 1
            && coordinator.state().items.front().manifest.id
                == "Samples/NewProject",
        "project replacement should reject every late worker update/result");
    require(
        coordinator.ignore("Samples/NewProject")
            && coordinator.state().items.front().ignored,
        "ignore should remain an explicit local-only row state");
}

void testReplacementAtGenerationLimitIsNotActionable()
{
    auto manifest = asset("Samples/GenerationLimit");
    manifest.generation =
        talas::project::maximumProjectAssetJsonInteger;
    talas::app::AssetRecoveryWorkbenchState state;
    state.items.push_back({
        manifest,
        talas::app::AssetRecoveryItemState::ReplacementCandidate,
        "root/limit.sample",
        "Different content",
        {resolution(
            manifest,
            talas::project::ProjectAssetResolutionMode::Replacement,
            "replacement/limit.bin",
            'b')},
        0,
    });
    const auto projection =
        talas::app::assetRecoveryWorkbenchProjection(state);
    require(
        projection.rows.size() == 1
            && !projection.rows.front().canReplace
            && projection.rows.front().message
                == "Replacement cannot advance beyond the maximum accepted generation",
        "replacement should be visibly disabled when the accepted generation cannot advance safely");
}

void testCandidateInspectionOwnsItsGenerationAndReportsTypedFailure()
{
    const auto manifest = asset("Samples/Locate");
    talas::app::AssetRecoveryWorkbenchCoordinator coordinator({{
        manifest,
        talas::app::AssetRecoveryItemState::Missing,
        "root/locate.sample",
        "Missing",
    }});

    const auto generation =
        coordinator.beginCandidateInspection(manifest.id);
    auto projection = talas::app::assetRecoveryWorkbenchProjection(
        coordinator.state());
    require(
        generation.has_value()
            && projection.canCancelSearch
            && !projection.canSearchFolder
            && !projection.canApplyRelocations
            && projection.statusText
                == "Inspecting Samples/Locate",
        "single-file inspection should own one cancellable worker generation and disable competing actions");

    talas::project::ProjectAssetCandidateResult cancelled;
    cancelled.error = talas::project::ProjectAssetInspectionError{
        talas::project::ProjectAssetInspectionErrorCode::Cancelled,
        "Asset inspection cancelled",
        "chosen.bin",
    };
    require(
        coordinator.finishCandidateInspection(
            *generation,
            manifest.id,
            std::move(cancelled)),
        "current candidate inspection should publish its typed cancellation");
    projection = talas::app::assetRecoveryWorkbenchProjection(
        coordinator.state());
    require(
        !projection.canCancelSearch
            && projection.rows.front().statusLabel == "Cancelled"
            && projection.rows.front().message
                == "Asset inspection cancelled"
            && projection.statusText
                == "1 asset needs recovery · 0 ready to apply",
        "cancelled inspection should retain the row and expose a non-colour recovery status");

    const auto replacementGeneration =
        coordinator.beginCandidateInspection(manifest.id);
    require(
        replacementGeneration.has_value(),
        "a retained row should support a later explicit inspection");
    talas::project::ProjectAssetCandidateResult replacement;
    replacement.resolution = resolution(
        manifest,
        talas::project::ProjectAssetResolutionMode::Replacement,
        "chosen.bin",
        'b');
    require(
        coordinator.finishCandidateInspection(
            *replacementGeneration,
            manifest.id,
            std::move(replacement))
            && coordinator.requestReplacementConfirmation(
                manifest.id),
        "different selected bytes should remain inert until the named replacement confirmation");

    const auto staleGeneration =
        coordinator.beginCandidateInspection(manifest.id);
    require(staleGeneration.has_value(), "stale inspection fixture should start");
    coordinator.replaceItems({{
        asset("Samples/New"),
        talas::app::AssetRecoveryItemState::Missing,
        "root/new.sample",
        "Missing",
    }});
    talas::project::ProjectAssetCandidateResult late;
    late.resolution = resolution(
        manifest,
        talas::project::ProjectAssetResolutionMode::Relocation,
        "late.bin");
    require(
        !coordinator.finishCandidateInspection(
            *staleGeneration,
            manifest.id,
            std::move(late))
            && coordinator.state().items.front().manifest.id
                == "Samples/New",
        "project replacement should reject a late single-file inspection result");
}

void testCancelInvalidatesQueuedWorkerSuccess()
{
    const auto manifest = asset("Samples/CancelRace");
    talas::app::AssetRecoveryWorkbenchCoordinator coordinator({{
        manifest,
        talas::app::AssetRecoveryItemState::Missing,
        "root/cancel.fileName",
        "Missing",
    }});

    const auto searchGeneration = coordinator.beginSearch();
    require(
        coordinator.cancelActiveOperation()
            && !coordinator.state().searchActive
            && coordinator.state().items.front().state
                == talas::app::AssetRecoveryItemState::Cancelled,
        "Cancel should synchronously close the active search generation and retain a cancelled row");
    talas::project::ProjectAssetSearchResult lateSearch;
    lateSearch.matches.push_back({
        manifest.id,
        {resolution(
            manifest,
            talas::project::ProjectAssetResolutionMode::Relocation,
            "late-search.bin")},
    });
    require(
        !coordinator.finishSearch(
            searchGeneration,
            std::move(lateSearch))
            && coordinator.state().items.front().candidates.empty(),
        "queued search success must be rejected after the owner presses Cancel");

    coordinator.replaceItems({{
        manifest,
        talas::app::AssetRecoveryItemState::Missing,
        "root/cancel.fileName",
        "Missing",
    }});
    const auto inspectionGeneration =
        coordinator.beginCandidateInspection(manifest.id);
    require(
        inspectionGeneration.has_value()
            && coordinator.cancelActiveOperation()
            && coordinator.state().items.front().state
                == talas::app::AssetRecoveryItemState::Cancelled,
        "Cancel should synchronously close a selected-file inspection generation");
    talas::project::ProjectAssetCandidateResult lateInspection;
    lateInspection.resolution = resolution(
        manifest,
        talas::project::ProjectAssetResolutionMode::Relocation,
        "late-inspection.bin");
    require(
        !coordinator.finishCandidateInspection(
            *inspectionGeneration,
            manifest.id,
            std::move(lateInspection))
            && coordinator.state().items.front().candidates.empty(),
        "queued inspection success must be rejected after the owner presses Cancel");

    coordinator.replaceItems({{
        manifest,
        talas::app::AssetRecoveryItemState::Changed,
        "root/cancel.fileName",
        "Changed",
    }});
    const auto inventoryGeneration =
        coordinator.beginInventoryInspection();
    require(
        coordinator.cancelActiveOperation()
            && coordinator.state().items.front().state
                == talas::app::AssetRecoveryItemState::Cancelled,
        "Cancel should synchronously invalidate a complete-project inventory generation");
    talas::app::AssetRecoveryInventoryResult lateInventory;
    lateInventory.items.push_back({
        manifest,
        talas::app::AssetRecoveryItemState::ReplacementCandidate,
        "root/cancel.fileName",
        "Late replacement",
        {resolution(
            manifest,
            talas::project::ProjectAssetResolutionMode::Replacement,
            "late-inventory.bin",
            'b')},
        0,
    });
    require(
        !coordinator.finishInventoryInspection(
            inventoryGeneration,
            std::move(lateInventory))
            && coordinator.state().items.front().candidates.empty(),
        "queued inventory success must be rejected after the owner presses Cancel");
}

void testFreshInventorySupersedesStaleCandidateCache()
{
    const auto manifest = asset("Samples/FreshInventory");
    talas::app::AssetRecoveryWorkbenchCoordinator coordinator({{
        manifest,
        talas::app::AssetRecoveryItemState::ReplacementCandidate,
        "root/fresh.fileName",
        "Cached B",
        {resolution(
            manifest,
            talas::project::ProjectAssetResolutionMode::Replacement,
            "candidate-b.bin",
            'b')},
        0,
    }});

    const auto changedGeneration =
        coordinator.beginInventoryInspection();
    talas::app::AssetRecoveryInventoryResult changedInventory;
    changedInventory.items.push_back({
        manifest,
        talas::app::AssetRecoveryItemState::ReplacementCandidate,
        "root/fresh.fileName",
        "Fresh C",
        {resolution(
            manifest,
            talas::project::ProjectAssetResolutionMode::Replacement,
            "candidate-c.bin",
            'c')},
        0,
    });
    require(
        coordinator.finishInventoryInspection(
            changedGeneration,
            std::move(changedInventory))
            && coordinator.state().items.front().message == "Fresh C"
            && coordinator.state().items.front().candidates.front()
                    .asset.contentHash
                == std::string(64, 'c'),
        "fresh inventory bytes C must supersede a same-manifest cached replacement B");

    const auto missingGeneration =
        coordinator.beginInventoryInspection();
    talas::app::AssetRecoveryInventoryResult missingInventory;
    missingInventory.items.push_back({
        manifest,
        talas::app::AssetRecoveryItemState::Missing,
        "root/fresh.fileName",
        "Fresh missing state",
    });
    require(
        coordinator.finishInventoryInspection(
            missingGeneration,
            std::move(missingInventory))
            && coordinator.state().items.front().state
                == talas::app::AssetRecoveryItemState::Missing
            && coordinator.state().items.front().candidates.empty(),
        "fresh missing state must clear the prior replacement candidate cache");
}
}

int main()
{
    testInventorySurfacesEveryChangedRegularAsset();
    testProjectionSeparatesResolutionAndReplacementActions();
    testProgressAccessibilityAndCompactPolicyRemainStable();
    testCoordinatorRejectsLateResultsAndKeepsReplacementExplicit();
    testCandidateInspectionOwnsItsGenerationAndReportsTypedFailure();
    testCancelInvalidatesQueuedWorkerSuccess();
    testFreshInventorySupersedesStaleCandidateCache();
    testReplacementAtGenerationLimitIsNotActionable();
    return 0;
}
