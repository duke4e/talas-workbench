#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace talas::app
{
enum class WorkbenchSurfaceStateKind
{
    Content,
    Empty,
    Loading,
    Unavailable,
    Stale,
    Error,
    Success,
};

enum class WorkbenchSurfaceSeverity
{
    Neutral,
    Progress,
    Warning,
    Error,
    Success,
};

enum class WorkbenchSurfaceAnnouncement
{
    None,
    Polite,
    Assertive,
};

struct WorkbenchSurfaceActionEvidence
{
    std::string id;
    std::string label;
    bool enabled = false;
};

struct WorkbenchSurfaceStateEvidence
{
    WorkbenchSurfaceStateKind kind = WorkbenchSurfaceStateKind::Content;
    std::string title;
    std::string explanation;
    std::optional<WorkbenchSurfaceActionEvidence> action;
    bool retainsUsableContent = false;
};

class WorkbenchSurfaceState;

class WorkbenchSurfaceAction final
{
public:
    [[nodiscard]] const std::string& id() const noexcept;
    [[nodiscard]] const std::string& label() const noexcept;
    [[nodiscard]] bool enabled() const noexcept;

    [[nodiscard]] bool operator==(
        const WorkbenchSurfaceAction&) const = default;

private:
    friend class WorkbenchSurfaceState;
    friend std::optional<WorkbenchSurfaceState>
    workbenchSurfaceState(const WorkbenchSurfaceStateEvidence&);
    WorkbenchSurfaceAction(std::string id, std::string label, bool enabled);

    std::string id_;
    std::string label_;
    bool enabled_ = false;
};

class WorkbenchSurfaceState final
{
public:
    [[nodiscard]] WorkbenchSurfaceStateKind kind() const noexcept;
    [[nodiscard]] WorkbenchSurfaceSeverity severity() const noexcept;
    [[nodiscard]] WorkbenchSurfaceAnnouncement announcement() const noexcept;
    [[nodiscard]] const std::string& title() const noexcept;
    [[nodiscard]] const std::string& explanation() const noexcept;
    [[nodiscard]] const std::optional<WorkbenchSurfaceAction>& action() const noexcept;
    [[nodiscard]] bool retainsUsableContent() const noexcept;

    [[nodiscard]] bool operator==(const WorkbenchSurfaceState&) const = default;

private:
    friend std::optional<WorkbenchSurfaceState>
    workbenchSurfaceState(const WorkbenchSurfaceStateEvidence&);

    WorkbenchSurfaceState(
        WorkbenchSurfaceStateKind kind,
        WorkbenchSurfaceSeverity severity,
        WorkbenchSurfaceAnnouncement announcement,
        std::string title,
        std::string explanation,
        std::optional<WorkbenchSurfaceAction> action,
        bool retainsUsableContent);

    WorkbenchSurfaceStateKind kind_ = WorkbenchSurfaceStateKind::Content;
    WorkbenchSurfaceSeverity severity_ = WorkbenchSurfaceSeverity::Neutral;
    WorkbenchSurfaceAnnouncement announcement_ =
        WorkbenchSurfaceAnnouncement::None;
    std::string title_;
    std::string explanation_;
    std::optional<WorkbenchSurfaceAction> action_;
    bool retainsUsableContent_ = false;
};

class WorkbenchSurfaceStateSource
{
public:
    virtual ~WorkbenchSurfaceStateSource() = default;

    [[nodiscard]] virtual const std::optional<WorkbenchSurfaceState>&
    workbenchSurfaceStateForContextHelp() const noexcept = 0;
};

[[nodiscard]] std::optional<WorkbenchSurfaceState>
workbenchSurfaceState(const WorkbenchSurfaceStateEvidence& evidence);

[[nodiscard]] std::string_view workbenchSurfaceStateName(
    WorkbenchSurfaceStateKind kind) noexcept;

[[nodiscard]] std::string_view workbenchSurfaceStateSymbol(
    WorkbenchSurfaceStateKind kind) noexcept;

[[nodiscard]] bool shouldAnnounceWorkbenchSurfaceState(
    const std::optional<WorkbenchSurfaceState>& previous,
    const WorkbenchSurfaceState& next) noexcept;
}
