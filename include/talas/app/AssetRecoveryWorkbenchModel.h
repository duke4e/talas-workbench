#pragma once

#include "talas/project/ProjectAssetResolver.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace talas::app
{
enum class AssetRecoveryItemState
{
    Missing,
    Inaccessible,
    Changed,
    Unverified,
    Searching,
    ExactMatch,
    InitialInspection,
    Ambiguous,
    ReplacementCandidate,
    CapacityExceeded,
    Cancelled,
    Ignored,
};

struct AssetRecoveryWorkbenchItem
{
    talas::project::ProjectAsset manifest;
    AssetRecoveryItemState state = AssetRecoveryItemState::Missing;
    std::string structuralSourcePath;
    std::string message;
    std::vector<talas::project::ProjectAssetResolution> candidates;
    std::optional<std::size_t> selectedCandidateIndex;
    bool ignored = false;
};

struct AssetRecoveryInventoryEntry
{
    talas::project::ProjectAsset manifest;
    std::string structuralSourcePath;
};

enum class AssetRecoveryInventoryStatus
{
    Completed,
    Cancelled,
};

struct AssetRecoveryInventoryResult
{
    AssetRecoveryInventoryStatus status =
        AssetRecoveryInventoryStatus::Completed;
    std::vector<AssetRecoveryWorkbenchItem> items;
};

[[nodiscard]] AssetRecoveryInventoryResult
inspectAssetRecoveryInventory(
    std::span<const AssetRecoveryInventoryEntry> entries,
    const std::optional<std::filesystem::path>& projectFilePath = std::nullopt,
    talas::project::ProjectAssetInspectionCancellationCheck shouldCancel = {});

enum class AssetRecoveryOperationKind
{
    None,
    InventoryInspection,
    CandidateInspection,
    FolderSearch,
};

struct AssetRecoveryWorkbenchState
{
    std::vector<AssetRecoveryWorkbenchItem> items;
    bool searchActive = false;
    AssetRecoveryOperationKind operationKind =
        AssetRecoveryOperationKind::None;
    std::optional<std::string> operationAssetId;
    talas::project::ProjectAssetSearchProgress progress;
    std::optional<std::string> replacementConfirmationAssetId;
};

struct AssetRecoveryWorkbenchRow
{
    std::string assetId;
    std::string kindLabel;
    std::string generationLabel;
    std::string shortHash;
    std::string storedPath;
    std::string structuralSourcePath;
    std::string statusLabel;
    std::string message;
    std::string accessibleDescription;
    bool searchEligible = false;
    bool canLocateFile = false;
    bool canChooseCandidate = false;
    bool selectedForApply = false;
    bool canReplace = false;
    bool canIgnore = false;
};

struct AssetRecoveryWorkbenchProjection
{
    std::vector<AssetRecoveryWorkbenchRow> rows;
    std::string statusText;
    std::size_t unresolvedCount = 0;
    std::size_t applyCount = 0;
    bool canSearchFolder = false;
    bool canApplyRelocations = false;
    bool canCancelSearch = false;
};

enum class AssetRecoveryLayoutMode
{
    Compact,
    Wide,
};

enum class AssetRecoveryFocusTarget
{
    Rows,
    LocateFile,
    SearchFolder,
    ApplyRelocations,
    Replace,
    Ignore,
    CancelSearch,
};

[[nodiscard]] AssetRecoveryWorkbenchProjection
assetRecoveryWorkbenchProjection(
    const AssetRecoveryWorkbenchState& state);

[[nodiscard]] std::vector<talas::project::ProjectAssetResolution>
assetRecoveryApplyBatch(const AssetRecoveryWorkbenchState& state);

[[nodiscard]] AssetRecoveryLayoutMode assetRecoveryLayoutMode(
    int availableWidth) noexcept;

[[nodiscard]] std::vector<AssetRecoveryFocusTarget>
assetRecoveryKeyboardOrder();

class AssetRecoveryWorkbenchCoordinator
{
public:
    explicit AssetRecoveryWorkbenchCoordinator(
        std::vector<AssetRecoveryWorkbenchItem> items = {});

    [[nodiscard]] const AssetRecoveryWorkbenchState& state() const noexcept;
    void replaceItems(std::vector<AssetRecoveryWorkbenchItem> items);
    [[nodiscard]] std::uint64_t beginInventoryInspection();
    [[nodiscard]] bool finishInventoryInspection(
        std::uint64_t generation,
        AssetRecoveryInventoryResult result);
    [[nodiscard]] std::uint64_t beginSearch();
    [[nodiscard]] std::optional<std::uint64_t>
    beginCandidateInspection(std::string_view assetId);
    [[nodiscard]] bool publishSearchProgress(
        std::uint64_t generation,
        talas::project::ProjectAssetSearchProgress progress);
    [[nodiscard]] bool finishSearch(
        std::uint64_t generation,
        talas::project::ProjectAssetSearchResult result);
    [[nodiscard]] bool finishCandidateInspection(
        std::uint64_t generation,
        std::string_view assetId,
        talas::project::ProjectAssetCandidateResult result);
    [[nodiscard]] bool cancelActiveOperation();
    [[nodiscard]] bool setCandidate(
        std::string_view assetId,
        talas::project::ProjectAssetResolution candidate);
    [[nodiscard]] bool selectCandidate(
        std::string_view assetId,
        std::size_t candidateIndex);
    [[nodiscard]] bool ignore(std::string_view assetId);
    [[nodiscard]] bool requestReplacementConfirmation(
        std::string_view assetId);
    [[nodiscard]] std::optional<talas::project::ProjectAssetResolution>
    confirmedReplacementCandidate() const;

private:
    struct SearchPriorState
    {
        std::string assetId;
        AssetRecoveryItemState state =
            AssetRecoveryItemState::Missing;
        std::string message;
    };

    AssetRecoveryWorkbenchState state_;
    std::vector<SearchPriorState> searchPriorStates_;
    std::uint64_t generation_ = 0;
};
}
