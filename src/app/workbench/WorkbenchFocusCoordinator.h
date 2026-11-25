#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace talas::app::workbench
{
enum class WorkbenchFocusRegion : std::uint8_t
{
    Toolbar,
    Library,
    PrimaryGraph,
    SecondaryGraph,
    RightWorkspace,
    LowerWorkspace,
    StatusAndHelp,
};

struct WorkbenchFocusIdentity
{
    WorkbenchFocusRegion region = WorkbenchFocusRegion::PrimaryGraph;
    std::string itemId;

    bool operator==(const WorkbenchFocusIdentity&) const = default;
};

struct WorkbenchFocusCandidate
{
    WorkbenchFocusIdentity identity;
    std::uint32_t orderWithinRegion = 0;
    bool visible = true;
    bool enabled = true;
};

[[nodiscard]] std::string workbenchStableFocusItemId(
    std::string_view category,
    std::span<const std::string_view> fields);

[[nodiscard]] std::optional<std::vector<WorkbenchFocusIdentity>>
workbenchVisibleFocusOrder(
    std::span<const WorkbenchFocusCandidate> candidates);

class WorkbenchFocusCoordinator
{
public:
    void capture(std::optional<WorkbenchFocusIdentity> identity);
    void clear() noexcept;

    [[nodiscard]] const std::optional<WorkbenchFocusIdentity>& captured()
        const noexcept;

    [[nodiscard]] std::optional<WorkbenchFocusIdentity> restorationTarget(
        std::span<const WorkbenchFocusCandidate> candidates,
        WorkbenchFocusRegion preferredRegion =
            WorkbenchFocusRegion::PrimaryGraph) const;

private:
    std::optional<WorkbenchFocusIdentity> captured_;
};
}
