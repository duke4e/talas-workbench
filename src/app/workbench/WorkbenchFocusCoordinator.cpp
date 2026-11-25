#include "app/workbench/WorkbenchFocusCoordinator.h"

#include <algorithm>
#include <cstdlib>
#include <set>
#include <tuple>

namespace talas::app::workbench
{
namespace
{
constexpr std::size_t maximumFocusIdentityBytes = 4096;
constexpr std::size_t maximumFocusCandidates = 64;
constexpr std::uint64_t focusHashOffsetA = 14695981039346656037ULL;
constexpr std::uint64_t focusHashOffsetB = 1099511628211ULL;
constexpr std::uint64_t focusHashPrimeA = 1099511628211ULL;
constexpr std::uint64_t focusHashPrimeB = 14029467366897019727ULL;

void hashByte(std::uint64_t& hash, std::uint64_t prime, std::uint8_t byte)
{
    hash ^= byte;
    hash *= prime;
}

void hashSize(std::uint64_t& hash, std::uint64_t prime, std::size_t size)
{
    for (std::size_t shift = 0; shift < sizeof(size); ++shift)
    {
        hashByte(hash, prime, static_cast<std::uint8_t>(size & 0xffU));
        size >>= 8U;
    }
}

[[nodiscard]] std::string hex64(std::uint64_t value)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string result(16, '0');
    for (std::size_t index = 0; index < result.size(); ++index)
    {
        const auto shift = 4U * (result.size() - index - 1U);
        result[index] = digits[(value >> shift) & 0xfU];
    }
    return result;
}

[[nodiscard]] bool isKnownRegion(WorkbenchFocusRegion region) noexcept
{
    switch (region)
    {
    case WorkbenchFocusRegion::Toolbar:
    case WorkbenchFocusRegion::Library:
    case WorkbenchFocusRegion::PrimaryGraph:
    case WorkbenchFocusRegion::SecondaryGraph:
    case WorkbenchFocusRegion::RightWorkspace:
    case WorkbenchFocusRegion::LowerWorkspace:
    case WorkbenchFocusRegion::StatusAndHelp:
        return true;
    }
    return false;
}

[[nodiscard]] int regionIndex(WorkbenchFocusRegion region) noexcept
{
    return static_cast<int>(region);
}
}

std::string workbenchStableFocusItemId(
    std::string_view category,
    std::span<const std::string_view> fields)
{
    if (category.empty() || category.size() > 64 || fields.size() > 16)
        return {};
    auto hashA = focusHashOffsetA;
    auto hashB = focusHashOffsetB;
    const auto consume = [&](std::string_view field) {
        hashSize(hashA, focusHashPrimeA, field.size());
        hashSize(hashB, focusHashPrimeB, field.size());
        for (const auto character : field)
        {
            const auto byte = static_cast<std::uint8_t>(character);
            hashByte(hashA, focusHashPrimeA, byte);
            hashByte(hashB, focusHashPrimeB, byte);
        }
    };
    consume(category);
    for (const auto field : fields)
        consume(field);
    return std::string(category) + ":" + hex64(hashA) + hex64(hashB);
}

std::optional<std::vector<WorkbenchFocusIdentity>>
workbenchVisibleFocusOrder(
    std::span<const WorkbenchFocusCandidate> candidates)
{
    if (candidates.size() > maximumFocusCandidates)
        return std::nullopt;
    std::vector<WorkbenchFocusCandidate> accepted;
    accepted.reserve(candidates.size());
    std::set<std::pair<int, std::string>> identities;
    for (const auto& candidate : candidates)
    {
        if (!isKnownRegion(candidate.identity.region)
            || candidate.identity.itemId.empty()
            || candidate.identity.itemId.size() > maximumFocusIdentityBytes)
        {
            return std::nullopt;
        }
        const auto key = std::pair{
            regionIndex(candidate.identity.region),
            candidate.identity.itemId,
        };
        if (!identities.insert(key).second)
            return std::nullopt;
        if (candidate.visible && candidate.enabled)
            accepted.push_back(candidate);
    }
    std::ranges::sort(accepted, [](const auto& left, const auto& right) {
        return std::tuple{
                   regionIndex(left.identity.region),
                   left.orderWithinRegion,
                   left.identity.itemId,
               }
            < std::tuple{
                   regionIndex(right.identity.region),
                   right.orderWithinRegion,
                   right.identity.itemId,
               };
    });
    std::vector<WorkbenchFocusIdentity> result;
    result.reserve(accepted.size());
    for (auto& candidate : accepted)
        result.push_back(std::move(candidate.identity));
    return result;
}

void WorkbenchFocusCoordinator::capture(
    std::optional<WorkbenchFocusIdentity> identity)
{
    if (identity.has_value()
        && (!isKnownRegion(identity->region)
            || identity->itemId.empty()
            || identity->itemId.size() > maximumFocusIdentityBytes))
    {
        captured_.reset();
        return;
    }
    captured_ = std::move(identity);
}

void WorkbenchFocusCoordinator::clear() noexcept
{
    captured_.reset();
}

const std::optional<WorkbenchFocusIdentity>&
WorkbenchFocusCoordinator::captured() const noexcept
{
    return captured_;
}

std::optional<WorkbenchFocusIdentity>
WorkbenchFocusCoordinator::restorationTarget(
    std::span<const WorkbenchFocusCandidate> candidates,
    WorkbenchFocusRegion preferredRegion) const
{
    const auto order = workbenchVisibleFocusOrder(candidates);
    if (!order.has_value() || order->empty())
        return std::nullopt;

    if (captured_.has_value())
    {
        const auto exact = std::ranges::find(*order, *captured_);
        if (exact != order->end())
            return *exact;

        const auto sameRegion = std::ranges::find_if(
            *order,
            [&](const auto& item) {
                return item.region == captured_->region;
            });
        if (sameRegion != order->end())
            return *sameRegion;
    }

    const auto preferred = std::ranges::find_if(
        *order,
        [&](const auto& item) {
            return item.region == preferredRegion;
        });
    if (preferred != order->end())
        return *preferred;

    if (!captured_.has_value())
        return order->front();

    const auto capturedIndex = regionIndex(captured_->region);
    return *std::ranges::min_element(
        *order,
        [&](const auto& left, const auto& right) {
            const auto leftDistance = std::abs(
                regionIndex(left.region) - capturedIndex);
            const auto rightDistance = std::abs(
                regionIndex(right.region) - capturedIndex);
            return std::tuple{leftDistance, regionIndex(left.region), left.itemId}
                < std::tuple{rightDistance, regionIndex(right.region), right.itemId};
        });
}
}
