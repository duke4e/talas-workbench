#include "talas/app/WorkbenchSurfaceState.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace talas::app
{
namespace
{
constexpr std::size_t maximumTitleBytes = 160;
constexpr std::size_t maximumExplanationBytes = 640;
constexpr std::size_t maximumActionIdBytes = 96;
constexpr std::size_t maximumActionLabelBytes = 120;

bool validText(std::string_view text, std::size_t maximumBytes)
{
    if (text.empty() || text.size() > maximumBytes)
        return false;
    return std::ranges::none_of(text, [](unsigned char value) {
        return value == 0 || (value < 0x20 && value != '\t');
    });
}

std::optional<std::pair<WorkbenchSurfaceSeverity,
                        WorkbenchSurfaceAnnouncement>>
policyFor(WorkbenchSurfaceStateKind kind) noexcept
{
    switch (kind)
    {
    case WorkbenchSurfaceStateKind::Content:
        return std::pair{WorkbenchSurfaceSeverity::Neutral,
                         WorkbenchSurfaceAnnouncement::None};
    case WorkbenchSurfaceStateKind::Empty:
        return std::pair{WorkbenchSurfaceSeverity::Neutral,
                         WorkbenchSurfaceAnnouncement::Polite};
    case WorkbenchSurfaceStateKind::Loading:
        return std::pair{WorkbenchSurfaceSeverity::Progress,
                         WorkbenchSurfaceAnnouncement::Polite};
    case WorkbenchSurfaceStateKind::Unavailable:
        return std::pair{WorkbenchSurfaceSeverity::Warning,
                         WorkbenchSurfaceAnnouncement::Polite};
    case WorkbenchSurfaceStateKind::Stale:
        return std::pair{WorkbenchSurfaceSeverity::Warning,
                         WorkbenchSurfaceAnnouncement::Polite};
    case WorkbenchSurfaceStateKind::Error:
        return std::pair{WorkbenchSurfaceSeverity::Error,
                         WorkbenchSurfaceAnnouncement::Assertive};
    case WorkbenchSurfaceStateKind::Success:
        return std::pair{WorkbenchSurfaceSeverity::Success,
                         WorkbenchSurfaceAnnouncement::Polite};
    }
    return std::nullopt;
}
}

WorkbenchSurfaceAction::WorkbenchSurfaceAction(
    std::string id, std::string label, bool enabled)
    : id_(std::move(id)), label_(std::move(label)), enabled_(enabled)
{
}

const std::string& WorkbenchSurfaceAction::id() const noexcept
{
    return id_;
}

const std::string& WorkbenchSurfaceAction::label() const noexcept
{
    return label_;
}

bool WorkbenchSurfaceAction::enabled() const noexcept
{
    return enabled_;
}

WorkbenchSurfaceState::WorkbenchSurfaceState(
    WorkbenchSurfaceStateKind kind,
    WorkbenchSurfaceSeverity severity,
    WorkbenchSurfaceAnnouncement announcement,
    std::string title,
    std::string explanation,
    std::optional<WorkbenchSurfaceAction> action,
    bool retainsUsableContent)
    : kind_(kind),
      severity_(severity),
      announcement_(announcement),
      title_(std::move(title)),
      explanation_(std::move(explanation)),
      action_(std::move(action)),
      retainsUsableContent_(retainsUsableContent)
{
}

WorkbenchSurfaceStateKind WorkbenchSurfaceState::kind() const noexcept
{
    return kind_;
}

WorkbenchSurfaceSeverity WorkbenchSurfaceState::severity() const noexcept
{
    return severity_;
}

WorkbenchSurfaceAnnouncement WorkbenchSurfaceState::announcement() const noexcept
{
    return announcement_;
}

const std::string& WorkbenchSurfaceState::title() const noexcept
{
    return title_;
}

const std::string& WorkbenchSurfaceState::explanation() const noexcept
{
    return explanation_;
}

const std::optional<WorkbenchSurfaceAction>&
WorkbenchSurfaceState::action() const noexcept
{
    return action_;
}

bool WorkbenchSurfaceState::retainsUsableContent() const noexcept
{
    return retainsUsableContent_;
}

std::optional<WorkbenchSurfaceState>
workbenchSurfaceState(const WorkbenchSurfaceStateEvidence& evidence)
{
    const auto policy = policyFor(evidence.kind);
    if (!policy.has_value()
        || !validText(evidence.title, maximumTitleBytes)
        || !validText(evidence.explanation, maximumExplanationBytes))
    {
        return std::nullopt;
    }

    if ((evidence.kind == WorkbenchSurfaceStateKind::Empty
         || evidence.kind == WorkbenchSurfaceStateKind::Loading
         || evidence.kind == WorkbenchSurfaceStateKind::Unavailable
         || evidence.kind == WorkbenchSurfaceStateKind::Success)
        && evidence.retainsUsableContent)
    {
        return std::nullopt;
    }
    if (evidence.kind == WorkbenchSurfaceStateKind::Stale
        && !evidence.retainsUsableContent)
    {
        return std::nullopt;
    }
    if (evidence.kind == WorkbenchSurfaceStateKind::Content
        && !evidence.retainsUsableContent)
    {
        return std::nullopt;
    }

    std::optional<WorkbenchSurfaceAction> action;
    if (evidence.action.has_value())
    {
        if (!validText(evidence.action->id, maximumActionIdBytes)
            || !validText(evidence.action->label, maximumActionLabelBytes))
        {
            return std::nullopt;
        }
        action = WorkbenchSurfaceAction{
            evidence.action->id,
            evidence.action->label,
            evidence.action->enabled};
    }

    return WorkbenchSurfaceState{
        evidence.kind,
        policy->first,
        policy->second,
        evidence.title,
        evidence.explanation,
        std::move(action),
        evidence.retainsUsableContent,
    };
}

std::string_view workbenchSurfaceStateName(
    WorkbenchSurfaceStateKind kind) noexcept
{
    switch (kind)
    {
    case WorkbenchSurfaceStateKind::Content: return "Content";
    case WorkbenchSurfaceStateKind::Empty: return "Empty";
    case WorkbenchSurfaceStateKind::Loading: return "Loading";
    case WorkbenchSurfaceStateKind::Unavailable: return "Unavailable";
    case WorkbenchSurfaceStateKind::Stale: return "Stale";
    case WorkbenchSurfaceStateKind::Error: return "Error";
    case WorkbenchSurfaceStateKind::Success: return "Success";
    }
    return "Invalid";
}

std::string_view workbenchSurfaceStateSymbol(
    WorkbenchSurfaceStateKind kind) noexcept
{
    switch (kind)
    {
    case WorkbenchSurfaceStateKind::Content: return "CONTENT";
    case WorkbenchSurfaceStateKind::Empty: return "EMPTY";
    case WorkbenchSurfaceStateKind::Loading: return "LOADING";
    case WorkbenchSurfaceStateKind::Unavailable: return "UNAVAILABLE";
    case WorkbenchSurfaceStateKind::Stale: return "STALE";
    case WorkbenchSurfaceStateKind::Error: return "ERROR";
    case WorkbenchSurfaceStateKind::Success: return "SUCCESS";
    }
    return "INVALID";
}

bool shouldAnnounceWorkbenchSurfaceState(
    const std::optional<WorkbenchSurfaceState>& previous,
    const WorkbenchSurfaceState& next) noexcept
{
    if (next.announcement() == WorkbenchSurfaceAnnouncement::None)
        return false;
    if (!previous.has_value())
        return true;
    return previous->kind() != next.kind()
        || previous->title() != next.title()
        || previous->action() != next.action()
        || previous->retainsUsableContent()
            != next.retainsUsableContent();
}
}
