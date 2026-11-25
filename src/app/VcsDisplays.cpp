#include "talas/app/VcsDisplays.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace talas::app
{
std::vector<VcsDisplay> vcsOscilloscopeDisplays(const talas::project::ProjectDocument& document)
{
    // The display takes the Sound's name as its label.
    std::vector<VcsDisplay> displays;
    for (const auto& node : document.graph.nodes)
    {
        if (node.typeId == vcsOscilloscopeTypeId)
            displays.push_back({node.instanceId, node.instanceId});
    }
    return displays;
}

VcsScopeZoom vcsScopeZoomed(VcsScopeZoom zoom, VcsScopeButton button) noexcept
{
    switch (button)
    {
    case VcsScopeButton::TimeOut:
        zoom.frames = std::min(maximumVcsScopeFrames, zoom.frames * 2);
        break;
    case VcsScopeButton::TimeIn:
        zoom.frames = std::max(minimumVcsScopeFrames, zoom.frames / 2);
        break;
    case VcsScopeButton::AmplitudeOut:
        zoom.amplitude = std::min(maximumVcsScopeAmplitude, zoom.amplitude * 2.0);
        break;
    case VcsScopeButton::AmplitudeIn:
        zoom.amplitude = std::max(minimumVcsScopeAmplitude, zoom.amplitude / 2.0);
        break;
    }
    return zoom;
}

std::string_view vcsScopeButtonText(VcsScopeButton button) noexcept
{
    switch (button)
    {
    case VcsScopeButton::TimeOut:
        return ">|<";
    case VcsScopeButton::TimeIn:
        return "<|>";
    case VcsScopeButton::AmplitudeOut:
        return "v-^";
    case VcsScopeButton::AmplitudeIn:
        return "^-v";
    }
    return {};
}

VcsScopeHistory::VcsScopeHistory(std::size_t capacity)
    : ring_(std::max<std::size_t>(capacity, 1))
{
}

void VcsScopeHistory::clear() noexcept
{
    head_ = 0;
    size_ = 0;
    end_ = 0;
}

void VcsScopeHistory::push(double value)
{
    if (size_ < ring_.size())
    {
        ring_[(head_ + size_) % ring_.size()] = value;
        ++size_;
        return;
    }
    ring_[head_] = value;
    head_ = (head_ + 1) % ring_.size();
}

void VcsScopeHistory::add(std::uint64_t position, std::span<const double> frames)
{
    if (frames.empty())
        return;
    const auto last = position + frames.size();
    // A capture ending before the frames kept begin is of a new start.
    if (size_ > 0 && last <= end_ - size_)
        clear();
    if (size_ == 0)
        end_ = position;
    if (last <= end_)
        return;
    if (position > end_)
    {
        const auto gap = position - end_;
        if (gap >= ring_.size())
            clear();
        else
        {
            for (std::uint64_t missing = 0; missing < gap; ++missing)
                push(std::numeric_limits<double>::quiet_NaN());
        }
        end_ = position;
    }
    for (auto frame = static_cast<std::size_t>(end_ - position); frame < frames.size(); ++frame)
        push(frames[frame]);
    end_ = last;
}

std::vector<double> VcsScopeHistory::latest(std::size_t count) const
{
    const auto kept = std::min(count, size_);
    std::vector<double> frames(kept);
    const auto start = size_ - kept;
    for (std::size_t index = 0; index < kept; ++index)
        frames[index] = ring_[(head_ + start + index) % ring_.size()];
    return frames;
}

std::vector<VcsScopeColumn> vcsScopeColumns(std::span<const double> frames, std::size_t zoomFrames, std::size_t columns)
{
    std::vector<VcsScopeColumn> trace(columns);
    if (columns == 0 || zoomFrames == 0)
        return trace;
    // A history shorter than the zoom sits at the right, newest last.
    const auto shown = std::min(frames.size(), zoomFrames);
    const auto offset = zoomFrames - shown;
    const auto first = frames.size() - shown;
    for (std::size_t column = 0; column < columns; ++column)
    {
        const auto start = column * zoomFrames / columns;
        const auto stop = std::max(start + 1, (column + 1) * zoomFrames / columns);
        auto& out = trace[column];
        for (auto at = start; at < stop; ++at)
        {
            if (at < offset)
                continue;
            const auto value = frames[first + (at - offset)];
            if (!std::isfinite(value))
                continue;
            out.low = out.present ? std::min(out.low, value) : value;
            out.high = out.present ? std::max(out.high, value) : value;
            out.present = true;
        }
    }
    return trace;
}
}
