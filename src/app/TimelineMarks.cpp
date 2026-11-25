#include "talas/app/TimelineMarks.h"

#include <algorithm>

namespace talas::app
{
void TimelineMarks::mark(const std::string& clipId)
{
    marked_.insert(clipId);
}

void TimelineMarks::toggle(const std::string& clipId)
{
    if (!marked_.erase(clipId))
        marked_.insert(clipId);
}

void TimelineMarks::markAll(const std::vector<std::string>& clipIds)
{
    marked_.insert(clipIds.begin(), clipIds.end());
}

void TimelineMarks::clear() noexcept
{
    marked_.clear();
}

bool TimelineMarks::marked(const std::string& clipId) const
{
    return marked_.contains(clipId);
}

const std::set<std::string>& TimelineMarks::clipIds() const noexcept
{
    return marked_;
}

void TimelineMarks::playMarkedOnly() noexcept
{
    markedOnly_ = true;
}

void TimelineMarks::playAll() noexcept
{
    markedOnly_ = false;
}

bool TimelineMarks::playsMarkedOnly() const noexcept
{
    return markedOnly_;
}

std::optional<std::vector<std::string>> TimelineMarks::playedClips(const talas::project::ProjectScore& score) const
{
    if (!markedOnly_)
        return std::nullopt;
    std::vector<std::string> played;
    for (const auto& clip : score.clips)
    {
        if (marked_.contains(clip.clipId))
            played.push_back(clip.clipId);
    }
    return played;
}

TimelineClipLook timelineClipOutline(bool overlaps, bool selected, bool marked) noexcept
{
    return overlaps ? TimelineClipLook::Overlapping
        : selected  ? TimelineClipLook::Selected
        : marked    ? TimelineClipLook::Marked
                    : TimelineClipLook::Plain;
}

TimelineClipLook timelineClipText(bool marked) noexcept
{
    return marked ? TimelineClipLook::Marked : TimelineClipLook::Plain;
}
}
