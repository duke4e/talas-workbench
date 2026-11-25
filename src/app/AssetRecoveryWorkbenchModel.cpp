#include "talas/app/AssetRecoveryWorkbenchModel.h"

#include "talas/project/ProjectAssetPaths.h"

#include <algorithm>
#include <sstream>
#include <string_view>

namespace talas::app
{
namespace
{
[[nodiscard]] std::string_view kindLabel(
    talas::project::ProjectAssetKind kind)
{
    switch (kind)
    {
    case talas::project::ProjectAssetKind::Table: return "Table";
    case talas::project::ProjectAssetKind::Binary: return "Binary";
    case talas::project::ProjectAssetKind::Audio: return "Audio";
    case talas::project::ProjectAssetKind::PartialSpectrum:
        return "Partial spectrum";
    }
    return "Asset";
}

[[nodiscard]] std::string_view statusLabel(
    AssetRecoveryItemState state)
{
    switch (state)
    {
    case AssetRecoveryItemState::Missing: return "Missing";
    case AssetRecoveryItemState::Inaccessible: return "Inaccessible";
    case AssetRecoveryItemState::Changed: return "Changed";
    case AssetRecoveryItemState::Unverified: return "Needs first inspection";
    case AssetRecoveryItemState::Searching: return "Searching";
    case AssetRecoveryItemState::ExactMatch: return "Exact match";
    case AssetRecoveryItemState::InitialInspection: return "Ready for first inspection";
    case AssetRecoveryItemState::Ambiguous: return "Choose matching path";
    case AssetRecoveryItemState::ReplacementCandidate: return "Different content";
    case AssetRecoveryItemState::CapacityExceeded: return "Search limit reached";
    case AssetRecoveryItemState::Cancelled: return "Cancelled";
    case AssetRecoveryItemState::Ignored: return "Ignored this session";
    }
    return "Unavailable";
}

[[nodiscard]] std::string shortHash(std::string_view hash)
{
    if (hash.empty())
        return "unverified";
    if (hash.size() <= 16)
        return std::string{hash};
    return std::string{hash.substr(0, 8)} + "…"
        + std::string{hash.substr(hash.size() - 8)};
}

[[nodiscard]] bool isSearchEligible(
    const AssetRecoveryWorkbenchItem& item)
{
    if (item.ignored
        || item.manifest.disposition
            != talas::project::ProjectAssetDisposition::Referenced
        || !talas::project::isCanonicalProjectAssetContentHash(
            item.manifest.contentHash))
    {
        return false;
    }
    return item.state == AssetRecoveryItemState::Missing
        || item.state == AssetRecoveryItemState::Inaccessible
        || item.state == AssetRecoveryItemState::Changed
        || item.state == AssetRecoveryItemState::ReplacementCandidate
        || item.state == AssetRecoveryItemState::CapacityExceeded
        || item.state == AssetRecoveryItemState::Cancelled;
}

[[nodiscard]] const talas::project::ProjectAssetResolution*
selectedCandidate(const AssetRecoveryWorkbenchItem& item)
{
    if (!item.selectedCandidateIndex.has_value()
        || *item.selectedCandidateIndex >= item.candidates.size())
    {
        return nullptr;
    }
    return &item.candidates[*item.selectedCandidateIndex];
}

[[nodiscard]] bool canApply(
    const AssetRecoveryWorkbenchItem& item)
{
    const auto* selected = selectedCandidate(item);
    return selected != nullptr
        && (selected->mode
                == talas::project::ProjectAssetResolutionMode::Relocation
            || selected->mode
                == talas::project::ProjectAssetResolutionMode::InitialInspection);
}
}

AssetRecoveryInventoryResult inspectAssetRecoveryInventory(
    std::span<const AssetRecoveryInventoryEntry> entries,
    const std::optional<std::filesystem::path>& projectFilePath,
    talas::project::ProjectAssetInspectionCancellationCheck shouldCancel)
{
    AssetRecoveryInventoryResult result;
    const auto cancelled = [&] {
        return shouldCancel && shouldCancel();
    };
    for (const auto& entry : entries)
    {
        if (cancelled())
        {
            result.status = AssetRecoveryInventoryStatus::Cancelled;
            result.items.clear();
            return result;
        }
        if (entry.manifest.disposition
            != talas::project::ProjectAssetDisposition::Referenced)
        {
            continue;
        }

        const auto resolvedPath = talas::project::resolveProjectAssetPath(
            entry.manifest,
            projectFilePath);
        std::error_code statusError;
        const auto status = resolvedPath.empty()
            ? std::filesystem::file_status{
                  std::filesystem::file_type::none}
            : std::filesystem::status(resolvedPath, statusError);
        const auto missing = statusError
                == std::errc::no_such_file_or_directory
            || (!statusError
                && status.type()
                    == std::filesystem::file_type::not_found);
        if (missing)
        {
            result.items.push_back({
                entry.manifest,
                AssetRecoveryItemState::Missing,
                entry.structuralSourcePath,
                "Referenced asset is missing",
            });
            continue;
        }
        if (statusError || !std::filesystem::is_regular_file(status))
        {
            result.items.push_back({
                entry.manifest,
                AssetRecoveryItemState::Inaccessible,
                entry.structuralSourcePath,
                statusError
                    ? "Asset cannot be inspected: "
                        + statusError.message()
                    : "Asset path is not a regular file",
            });
            continue;
        }

        auto classified = talas::project::classifyProjectAssetCandidate(
            entry.manifest,
            resolvedPath,
            projectFilePath,
            shouldCancel);
        if (!classified.ok())
        {
            if (classified.error.has_value()
                && classified.error->code
                    == talas::project::ProjectAssetInspectionErrorCode::Cancelled)
            {
                result.status = AssetRecoveryInventoryStatus::Cancelled;
                result.items.clear();
                return result;
            }
            result.items.push_back({
                entry.manifest,
                AssetRecoveryItemState::Inaccessible,
                entry.structuralSourcePath,
                classified.error.has_value()
                    ? std::move(classified.error->message)
                    : std::string{"Asset cannot be inspected"},
            });
            continue;
        }

        auto resolution = std::move(*classified.resolution);
        if (resolution.mode
            == talas::project::ProjectAssetResolutionMode::Relocation)
        {
            continue;
        }
        const auto initial = resolution.mode
            == talas::project::ProjectAssetResolutionMode::InitialInspection;
        result.items.push_back({
            entry.manifest,
            initial
                ? AssetRecoveryItemState::InitialInspection
                : AssetRecoveryItemState::ReplacementCandidate,
            entry.structuralSourcePath,
            initial
                ? "First inspection ready to apply"
                : "Different content requires replacement confirmation",
            {std::move(resolution)},
            0,
        });
    }
    return result;
}

AssetRecoveryWorkbenchProjection assetRecoveryWorkbenchProjection(
    const AssetRecoveryWorkbenchState& state)
{
    AssetRecoveryWorkbenchProjection projection;
    projection.rows.reserve(state.items.size());
    projection.canCancelSearch = state.searchActive;
    for (const auto& item : state.items)
    {
        const auto ignored = item.ignored
            || item.state == AssetRecoveryItemState::Ignored;
        if (!ignored)
            ++projection.unresolvedCount;

        const auto* selected = selectedCandidate(item);
        const auto selectedForApply = !state.searchActive
            && !ignored && canApply(item);
        if (selectedForApply)
            ++projection.applyCount;
        const auto searchEligible = !state.searchActive
            && isSearchEligible(item);
        projection.canSearchFolder =
            projection.canSearchFolder || searchEligible;

        AssetRecoveryWorkbenchRow row;
        row.assetId = item.manifest.id;
        row.kindLabel = std::string{kindLabel(item.manifest.kind)};
        row.generationLabel = "generation "
            + std::to_string(item.manifest.generation);
        row.shortHash = shortHash(item.manifest.contentHash);
        row.storedPath = item.manifest.path;
        row.structuralSourcePath = item.structuralSourcePath;
        row.statusLabel = std::string{statusLabel(item.state)};
        row.message = item.message;
        row.searchEligible = searchEligible;
        row.canLocateFile = !state.searchActive && !ignored;
        row.canChooseCandidate = !state.searchActive && !ignored
            && item.candidates.size() > 1;
        row.selectedForApply = selectedForApply;
        row.canReplace = !state.searchActive && !ignored
            && selected != nullptr
            && selected->mode
                == talas::project::ProjectAssetResolutionMode::Replacement
            && item.manifest.generation
                < talas::project::maximumProjectAssetJsonInteger;
        if (selected != nullptr
            && selected->mode
                == talas::project::ProjectAssetResolutionMode::Replacement
            && item.manifest.generation
                >= talas::project::maximumProjectAssetJsonInteger)
        {
            row.message = "Replacement cannot advance beyond the maximum accepted generation";
        }
        row.canIgnore = !state.searchActive && !ignored;

        std::ostringstream accessible;
        accessible << row.assetId << ", " << row.kindLabel
                   << ", " << row.generationLabel
                   << ", hash " << row.shortHash
                   << ", " << row.statusLabel
                   << ", stored path " << row.storedPath;
        if (!row.structuralSourcePath.empty())
            accessible << ", used by " << row.structuralSourcePath;
        if (!row.message.empty())
            accessible << ". " << row.message;
        row.accessibleDescription = accessible.str();
        projection.rows.push_back(std::move(row));
    }

    projection.canApplyRelocations =
        !state.searchActive && projection.applyCount != 0;
    if (state.searchActive)
    {
        projection.canSearchFolder = false;
        if (state.operationKind
            == AssetRecoveryOperationKind::InventoryInspection)
        {
            projection.statusText = "Inspecting referenced project assets";
        }
        else if (state.operationKind
            == AssetRecoveryOperationKind::CandidateInspection)
        {
            projection.statusText = "Inspecting "
                + state.operationAssetId.value_or("selected asset");
        }
        else
        {
            projection.statusText = "Searching assets · "
                + std::to_string(state.progress.hashedFiles)
                + " files hashed · "
                + std::to_string(state.progress.visitedEntries)
                + " entries visited";
            if (!state.progress.currentRelativePath.empty())
            {
                projection.statusText += " · "
                    + state.progress.currentRelativePath;
            }
        }
    }
    else if (projection.unresolvedCount == 0)
    {
        projection.statusText = "No asset recovery required";
    }
    else
    {
        projection.statusText = std::to_string(projection.unresolvedCount)
            + (projection.unresolvedCount == 1
                   ? " asset needs recovery · "
                   : " assets need recovery · ")
            + std::to_string(projection.applyCount)
            + " ready to apply";
    }
    return projection;
}

std::vector<talas::project::ProjectAssetResolution>
assetRecoveryApplyBatch(const AssetRecoveryWorkbenchState& state)
{
    std::vector<talas::project::ProjectAssetResolution> batch;
    if (state.searchActive)
        return batch;
    for (const auto& item : state.items)
    {
        if (item.ignored
            || item.state == AssetRecoveryItemState::Ignored
            || !canApply(item))
        {
            continue;
        }
        batch.push_back(*selectedCandidate(item));
    }
    return batch;
}

AssetRecoveryLayoutMode assetRecoveryLayoutMode(
    int availableWidth) noexcept
{
    return availableWidth < 360
        ? AssetRecoveryLayoutMode::Compact
        : AssetRecoveryLayoutMode::Wide;
}

std::vector<AssetRecoveryFocusTarget> assetRecoveryKeyboardOrder()
{
    return {
        AssetRecoveryFocusTarget::Rows,
        AssetRecoveryFocusTarget::LocateFile,
        AssetRecoveryFocusTarget::SearchFolder,
        AssetRecoveryFocusTarget::ApplyRelocations,
        AssetRecoveryFocusTarget::Replace,
        AssetRecoveryFocusTarget::Ignore,
        AssetRecoveryFocusTarget::CancelSearch,
    };
}

AssetRecoveryWorkbenchCoordinator::AssetRecoveryWorkbenchCoordinator(
    std::vector<AssetRecoveryWorkbenchItem> items)
{
    state_.items = std::move(items);
}

const AssetRecoveryWorkbenchState&
AssetRecoveryWorkbenchCoordinator::state() const noexcept
{
    return state_;
}

void AssetRecoveryWorkbenchCoordinator::replaceItems(
    std::vector<AssetRecoveryWorkbenchItem> items)
{
    ++generation_;
    state_ = {};
    searchPriorStates_.clear();
    state_.items = std::move(items);
}

std::uint64_t
AssetRecoveryWorkbenchCoordinator::beginInventoryInspection()
{
    ++generation_;
    state_.searchActive = true;
    state_.operationKind = AssetRecoveryOperationKind::InventoryInspection;
    state_.operationAssetId.reset();
    state_.progress = {};
    state_.replacementConfirmationAssetId.reset();
    searchPriorStates_.clear();
    return generation_;
}

bool AssetRecoveryWorkbenchCoordinator::finishInventoryInspection(
    std::uint64_t generation,
    AssetRecoveryInventoryResult result)
{
    if (generation != generation_ || !state_.searchActive
        || state_.operationKind
            != AssetRecoveryOperationKind::InventoryInspection)
    {
        return false;
    }

    state_.searchActive = false;
    state_.operationKind = AssetRecoveryOperationKind::None;
    state_.operationAssetId.reset();
    state_.progress = {};
    state_.replacementConfirmationAssetId.reset();
    if (result.status == AssetRecoveryInventoryStatus::Cancelled)
    {
        for (auto& item : state_.items)
        {
            if (item.ignored)
                continue;
            item.candidates.clear();
            item.selectedCandidateIndex.reset();
            item.state = AssetRecoveryItemState::Cancelled;
            item.message = "Project asset inspection cancelled";
        }
        return true;
    }

    for (auto& inventoryItem : result.items)
    {
        const auto previous = std::ranges::find(
            state_.items,
            inventoryItem.manifest.id,
            [](const auto& item) -> const std::string& {
                return item.manifest.id;
            });
        if (previous != state_.items.end()
            && previous->manifest == inventoryItem.manifest
            && previous->ignored)
        {
            inventoryItem.ignored = true;
            inventoryItem.state = AssetRecoveryItemState::Ignored;
            inventoryItem.message = "Ignored for this session";
            inventoryItem.candidates.clear();
            inventoryItem.selectedCandidateIndex.reset();
        }
    }
    state_.items = std::move(result.items);
    return true;
}

std::uint64_t AssetRecoveryWorkbenchCoordinator::beginSearch()
{
    ++generation_;
    state_.searchActive = true;
    state_.operationKind = AssetRecoveryOperationKind::FolderSearch;
    state_.operationAssetId.reset();
    state_.progress = {};
    state_.replacementConfirmationAssetId.reset();
    searchPriorStates_.clear();
    for (auto& item : state_.items)
    {
        if (!isSearchEligible(item))
            continue;
        searchPriorStates_.push_back({
            item.manifest.id,
            item.state,
            item.message,
        });
        item.state = AssetRecoveryItemState::Searching;
        item.message = "Searching selected folder";
    }
    return generation_;
}

std::optional<std::uint64_t>
AssetRecoveryWorkbenchCoordinator::beginCandidateInspection(
    std::string_view assetId)
{
    const auto found = std::ranges::find(
        state_.items,
        assetId,
        [](const auto& item) -> const std::string& {
            return item.manifest.id;
        });
    if (state_.searchActive
        || found == state_.items.end()
        || found->ignored)
    {
        return std::nullopt;
    }
    ++generation_;
    state_.searchActive = true;
    state_.operationKind =
        AssetRecoveryOperationKind::CandidateInspection;
    state_.operationAssetId = std::string{assetId};
    state_.progress = {};
    state_.replacementConfirmationAssetId.reset();
    searchPriorStates_.clear();
    return generation_;
}

bool AssetRecoveryWorkbenchCoordinator::publishSearchProgress(
    std::uint64_t generation,
    talas::project::ProjectAssetSearchProgress progress)
{
    if (generation != generation_ || !state_.searchActive
        || state_.operationKind
            != AssetRecoveryOperationKind::FolderSearch)
        return false;
    state_.progress = std::move(progress);
    return true;
}

bool AssetRecoveryWorkbenchCoordinator::finishSearch(
    std::uint64_t generation,
    talas::project::ProjectAssetSearchResult result)
{
    if (generation != generation_ || !state_.searchActive
        || state_.operationKind
            != AssetRecoveryOperationKind::FolderSearch)
        return false;
    state_.searchActive = false;
    state_.operationKind = AssetRecoveryOperationKind::None;
    state_.operationAssetId.reset();
    state_.progress.visitedEntries = result.visitedEntries;
    state_.progress.hashedFiles = result.hashedFiles;

    const auto findItem = [&](std::string_view assetId)
        -> AssetRecoveryWorkbenchItem* {
        for (auto& item : state_.items)
        {
            if (item.manifest.id == assetId)
                return &item;
        }
        return nullptr;
    };

    for (const auto& prior : searchPriorStates_)
    {
        if (auto* item = findItem(prior.assetId); item != nullptr)
        {
            item->state = prior.state;
            item->message = prior.message;
        }
    }

    if (result.status == talas::project::ProjectAssetSearchStatus::Completed)
    {
        for (auto& match : result.matches)
        {
            auto* item = findItem(match.assetId);
            if (item == nullptr || item->ignored)
                continue;
            const auto hasReplacementCandidate =
                item->candidates.size() == 1
                && item->candidates.front().mode
                    == talas::project::ProjectAssetResolutionMode::Replacement;
            if (match.candidates.empty())
            {
                if (!hasReplacementCandidate)
                {
                    item->candidates.clear();
                    item->selectedCandidateIndex.reset();
                }
                continue;
            }
            item->candidates = std::move(match.candidates);
            item->ignored = false;
            if (item->candidates.size() == 1)
            {
                item->state = AssetRecoveryItemState::ExactMatch;
                item->selectedCandidateIndex = 0;
                item->message = "Exact SHA-256 match ready to apply";
            }
            else
            {
                item->state = AssetRecoveryItemState::Ambiguous;
                item->selectedCandidateIndex.reset();
                item->message = "Several exact SHA-256 matches require a path choice";
            }
        }
        searchPriorStates_.clear();
        return true;
    }

    const auto stateAfterFailure = [&] {
        switch (result.status)
        {
        case talas::project::ProjectAssetSearchStatus::Cancelled:
            return AssetRecoveryItemState::Cancelled;
        case talas::project::ProjectAssetSearchStatus::CapacityExceeded:
            return AssetRecoveryItemState::CapacityExceeded;
        case talas::project::ProjectAssetSearchStatus::InvalidRoot:
            return AssetRecoveryItemState::Inaccessible;
        case talas::project::ProjectAssetSearchStatus::Completed:
            break;
        }
        return AssetRecoveryItemState::Inaccessible;
    }();
    const auto failureMessage = result.diagnostics.empty()
        ? (result.status == talas::project::ProjectAssetSearchStatus::Cancelled
               ? std::string{"Asset search cancelled"}
               : std::string{"Asset search failed"})
        : result.diagnostics.back().message;
    for (const auto& prior : searchPriorStates_)
    {
        if (auto* item = findItem(prior.assetId);
            item != nullptr && !item->ignored)
        {
            item->state = stateAfterFailure;
            item->message = failureMessage;
        }
    }
    searchPriorStates_.clear();
    return true;
}

bool AssetRecoveryWorkbenchCoordinator::finishCandidateInspection(
    std::uint64_t generation,
    std::string_view assetId,
    talas::project::ProjectAssetCandidateResult result)
{
    if (generation != generation_ || !state_.searchActive
        || state_.operationKind
            != AssetRecoveryOperationKind::CandidateInspection
        || state_.operationAssetId
            != std::optional<std::string>{std::string{assetId}})
    {
        return false;
    }

    state_.searchActive = false;
    state_.operationKind = AssetRecoveryOperationKind::None;
    state_.operationAssetId.reset();
    if (result.ok())
        return setCandidate(assetId, std::move(*result.resolution));

    const auto found = std::ranges::find(
        state_.items,
        assetId,
        [](const auto& item) -> const std::string& {
            return item.manifest.id;
        });
    if (found == state_.items.end() || found->ignored)
        return false;
    found->candidates.clear();
    found->selectedCandidateIndex.reset();
    found->state = result.error.has_value()
            && result.error->code
                == talas::project::ProjectAssetInspectionErrorCode::Cancelled
        ? AssetRecoveryItemState::Cancelled
        : AssetRecoveryItemState::Inaccessible;
    found->message = result.error.has_value()
        ? std::move(result.error->message)
        : "Asset inspection failed";
    return true;
}

bool AssetRecoveryWorkbenchCoordinator::cancelActiveOperation()
{
    if (!state_.searchActive)
        return false;

    ++generation_;
    if (state_.operationKind
        == AssetRecoveryOperationKind::InventoryInspection)
    {
        for (auto& item : state_.items)
        {
            if (item.ignored)
                continue;
            item.candidates.clear();
            item.selectedCandidateIndex.reset();
            item.state = AssetRecoveryItemState::Cancelled;
            item.message = "Project asset inspection cancelled";
        }
    }
    else if (state_.operationKind == AssetRecoveryOperationKind::FolderSearch)
    {
        for (const auto& prior : searchPriorStates_)
        {
            const auto found = std::ranges::find(
                state_.items,
                prior.assetId,
                [](const auto& item) -> const std::string& {
                    return item.manifest.id;
                });
            if (found == state_.items.end() || found->ignored)
                continue;
            found->candidates.clear();
            found->selectedCandidateIndex.reset();
            found->state = AssetRecoveryItemState::Cancelled;
            found->message = "Asset search cancelled";
        }
    }
    else if (state_.operationKind
             == AssetRecoveryOperationKind::CandidateInspection)
    {
        const auto found = std::ranges::find(
            state_.items,
            state_.operationAssetId.value_or(std::string{}),
            [](const auto& item) -> const std::string& {
                return item.manifest.id;
            });
        if (found != state_.items.end() && !found->ignored)
        {
            found->candidates.clear();
            found->selectedCandidateIndex.reset();
            found->state = AssetRecoveryItemState::Cancelled;
            found->message = "Asset inspection cancelled";
        }
    }

    state_.searchActive = false;
    state_.operationKind = AssetRecoveryOperationKind::None;
    state_.operationAssetId.reset();
    state_.progress = {};
    state_.replacementConfirmationAssetId.reset();
    searchPriorStates_.clear();
    return true;
}

bool AssetRecoveryWorkbenchCoordinator::setCandidate(
    std::string_view assetId,
    talas::project::ProjectAssetResolution candidate)
{
    for (auto& item : state_.items)
    {
        if (item.manifest.id != assetId)
            continue;
        if (candidate.asset.id != item.manifest.id
            || candidate.expectedGeneration != item.manifest.generation
            || candidate.expectedContentHash
                != item.manifest.contentHash)
        {
            return false;
        }
        item.candidates = {std::move(candidate)};
        item.selectedCandidateIndex = 0;
        item.ignored = false;
        state_.replacementConfirmationAssetId.reset();
        switch (item.candidates.front().mode)
        {
        case talas::project::ProjectAssetResolutionMode::Relocation:
            item.state = AssetRecoveryItemState::ExactMatch;
            item.message = "Exact SHA-256 match ready to apply";
            break;
        case talas::project::ProjectAssetResolutionMode::InitialInspection:
            item.state = AssetRecoveryItemState::InitialInspection;
            item.message = "First inspection ready to apply";
            break;
        case talas::project::ProjectAssetResolutionMode::Replacement:
            item.state = AssetRecoveryItemState::ReplacementCandidate;
            item.message = "Different content requires replacement confirmation";
            break;
        }
        return true;
    }
    return false;
}

bool AssetRecoveryWorkbenchCoordinator::selectCandidate(
    std::string_view assetId,
    std::size_t candidateIndex)
{
    for (auto& item : state_.items)
    {
        if (item.manifest.id != assetId)
            continue;
        if (item.ignored || candidateIndex >= item.candidates.size())
            return false;
        item.selectedCandidateIndex = candidateIndex;
        state_.replacementConfirmationAssetId.reset();
        return true;
    }
    return false;
}

bool AssetRecoveryWorkbenchCoordinator::ignore(std::string_view assetId)
{
    for (auto& item : state_.items)
    {
        if (item.manifest.id != assetId)
            continue;
        item.ignored = true;
        item.state = AssetRecoveryItemState::Ignored;
        item.candidates.clear();
        item.selectedCandidateIndex.reset();
        if (state_.replacementConfirmationAssetId
            == std::optional<std::string>{std::string{assetId}})
        {
            state_.replacementConfirmationAssetId.reset();
        }
        return true;
    }
    return false;
}

bool AssetRecoveryWorkbenchCoordinator::requestReplacementConfirmation(
    std::string_view assetId)
{
    for (const auto& item : state_.items)
    {
        if (item.manifest.id != assetId || item.ignored)
            continue;
        const auto* selected = selectedCandidate(item);
        if (selected == nullptr
            || selected->mode
                != talas::project::ProjectAssetResolutionMode::Replacement)
        {
            return false;
        }
        state_.replacementConfirmationAssetId = std::string{assetId};
        return true;
    }
    return false;
}

std::optional<talas::project::ProjectAssetResolution>
AssetRecoveryWorkbenchCoordinator::confirmedReplacementCandidate() const
{
    if (!state_.replacementConfirmationAssetId.has_value())
        return std::nullopt;
    for (const auto& item : state_.items)
    {
        if (item.manifest.id
                != *state_.replacementConfirmationAssetId
            || item.ignored)
        {
            continue;
        }
        const auto* selected = selectedCandidate(item);
        if (selected != nullptr
            && selected->mode
                == talas::project::ProjectAssetResolutionMode::Replacement)
        {
            return *selected;
        }
    }
    return std::nullopt;
}
}
