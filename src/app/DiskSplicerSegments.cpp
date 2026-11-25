#include "talas/app/DiskSplicerSegments.h"

#include "talas/core/StaticResourceList.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <sstream>

namespace talas::app
{
namespace
{
// Segments of no samples dropped, and a segment that goes on where the one
// before it ends in the same file joined to it.
std::vector<SplicedSegment> joined(std::vector<SplicedSegment> segments)
{
    std::vector<SplicedSegment> result;
    for (auto& segment : segments)
    {
        if (segment.length() == 0)
            continue;
        if (!result.empty() && result.back().file == segment.file && result.back().last == segment.first)
            result.back().last = segment.last;
        else
            result.push_back(std::move(segment));
    }
    return result;
}

std::size_t lengthOf(const std::vector<SplicedSegment>& segments)
{
    std::size_t total = 0;
    for (const auto& segment : segments)
        total += segment.length();
    return total;
}
}

std::optional<std::vector<SplicedSegment>> readSplicedSegments(
    std::string_view text, const SampleFileFrames& frames, std::string& error)
{
    std::vector<SplicedSegment> segments;
    std::istringstream lines{std::string{text}};
    std::size_t count = 0;
    for (std::string line; std::getline(lines, line);)
    {
        const auto words = talas::core::splitQuotedWords(line);
        if (!words)
        {
            error = "segment " + std::to_string(count + 1) + " leaves a quote open";
            return std::nullopt;
        }
        const auto& tokens = *words;
        if (tokens.empty())
            continue;
        ++count;
        const auto held = frames ? frames(tokens.front()) : std::nullopt;
        if (!held || *held == 0)
        {
            error = "segment " + std::to_string(count) + " names no sample file the project holds: " + tokens.front();
            return std::nullopt;
        }
        const auto whole = [](const std::string& word, std::size_t& value) {
            const auto [end, failure] = std::from_chars(word.data(), word.data() + word.size(), value);
            return failure == std::errc{} && end == word.data() + word.size();
        };
        SplicedSegment segment{tokens.front(), 0, *held};
        const auto read = tokens.size() == 1
            || (tokens.size() == 3 && whole(tokens[1], segment.first) && whole(tokens[2], segment.last));
        if (!read || segment.last <= segment.first || segment.last > *held)
        {
            error = "segment " + std::to_string(count)
                + " takes a file, then optionally its first sample and the sample after its last, within the file";
            return std::nullopt;
        }
        segments.push_back(std::move(segment));
    }
    error.clear();
    return segments;
}

std::string splicedSegmentsText(const std::vector<SplicedSegment>& segments, const SampleFileFrames& frames)
{
    std::string text;
    for (const auto& segment : segments)
    {
        if (!text.empty())
            text += '\n';
        text += talas::core::quotedWord(segment.file);
        const auto held = frames ? frames(segment.file) : std::nullopt;
        if (segment.first != 0 || !held || segment.last != *held)
            text += ' ' + std::to_string(segment.first) + ' ' + std::to_string(segment.last);
    }
    return text;
}

DiskSplicerSegments::DiskSplicerSegments(std::vector<SplicedSegment> segments)
    : segments_(joined(std::move(segments)))
{
}

std::size_t DiskSplicerSegments::frames() const noexcept
{
    return lengthOf(segments_);
}

void DiskSplicerSegments::select(std::size_t start, std::size_t end)
{
    const auto total = frames();
    start = std::min(start, total);
    end = std::min(end, total);
    if (end < start)
        std::swap(start, end);
    selection_ = {start, end};
}

void DiskSplicerSegments::selectAll()
{
    selection_ = {0, frames()};
}

std::vector<SplicedSegment> DiskSplicerSegments::between(std::size_t start, std::size_t end) const
{
    std::vector<SplicedSegment> result;
    std::size_t offset = 0;
    for (const auto& segment : segments_)
    {
        const auto from = std::max(start, offset);
        const auto to = std::min(end, offset + segment.length());
        if (from < to)
            result.push_back({segment.file, segment.first + (from - offset), segment.first + (to - offset)});
        offset += segment.length();
    }
    return result;
}

std::vector<SplicedSegment> DiskSplicerSegments::selected() const
{
    return between(selection_.start, selection_.end);
}

void DiskSplicerSegments::replace(std::size_t start, std::size_t end, const std::vector<SplicedSegment>& with)
{
    auto result = between(0, start);
    result.insert(result.end(), with.begin(), with.end());
    const auto after = between(end, frames());
    result.insert(result.end(), after.begin(), after.end());
    segments_ = joined(std::move(result));
}

bool DiskSplicerSegments::cut()
{
    return copy() && clear();
}

bool DiskSplicerSegments::copy()
{
    if (selection_.empty())
        return false;
    clipboard_ = selected();
    return true;
}

bool DiskSplicerSegments::paste()
{
    if (clipboard_.empty())
        return false;
    replace(selection_.start, selection_.end, clipboard_);
    selection_ = {selection_.start, selection_.start + lengthOf(clipboard_)};
    return true;
}

bool DiskSplicerSegments::clear()
{
    if (selection_.empty())
        return false;
    replace(selection_.start, selection_.end, {});
    selection_ = {selection_.start, selection_.start};
    return true;
}

bool DiskSplicerSegments::trim()
{
    if (selection_.empty() || selection_.length() == frames())
        return false;
    segments_ = joined(selected());
    selection_ = {0, frames()};
    return true;
}

bool DiskSplicerSegments::pasteFile(std::string file, std::size_t fileFrames)
{
    if (file.empty() || fileFrames == 0)
        return false;
    replace(selection_.start, selection_.end, {{std::move(file), 0, fileFrames}});
    selection_ = {selection_.start, selection_.start + fileFrames};
    return true;
}

std::optional<DiskSplicerSegments::Source> DiskSplicerSegments::sourceAt(std::size_t frame) const
{
    std::size_t offset = 0;
    for (std::size_t index = 0; index < segments_.size(); ++index)
    {
        const auto length = segments_[index].length();
        if (frame < offset + length)
            return Source{index, segments_[index].first + (frame - offset)};
        offset += length;
    }
    return std::nullopt;
}

std::vector<std::size_t> DiskSplicerSegments::segmentStarts() const
{
    std::vector<std::size_t> starts;
    std::size_t offset = 0;
    for (const auto& segment : segments_)
    {
        starts.push_back(offset);
        offset += segment.length();
    }
    return starts;
}

std::vector<std::vector<double>> splicedSamples(
    const std::vector<SplicedSegment>& segments, const SampleFileChannels& channels)
{
    const auto fileOf = [&](const SplicedSegment& segment) {
        const auto* file = channels ? channels(segment.file) : nullptr;
        return file != nullptr && !file->empty() ? file : nullptr;
    };
    std::size_t width = 1;
    for (const auto& segment : segments)
    {
        if (const auto* file = fileOf(segment))
            width = std::max(width, std::min<std::size_t>(file->size(), 2));
    }
    std::vector<std::vector<double>> result(width);
    for (const auto& segment : segments)
    {
        const auto* file = fileOf(segment);
        for (std::size_t channel = 0; channel < width; ++channel)
        {
            auto& out = result[channel];
            std::size_t copied = 0;
            if (file != nullptr)
            {
                const auto& data = (*file)[std::min(channel, file->size() - 1)];
                const auto last = std::min(segment.last, data.size());
                const auto first = std::min(segment.first, last);
                out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(first),
                    data.begin() + static_cast<std::ptrdiff_t>(last));
                copied = last - first;
            }
            out.insert(out.end(), segment.length() - copied, 0.0);
        }
    }
    return result;
}

SampleEditorView splicedViewBetween(SampleEditorView view, std::size_t from, std::size_t to, std::size_t total) noexcept
{
    if (to < from)
        std::swap(from, to);
    view.start = std::min(from, total);
    view.frames = std::max<std::size_t>(std::min(to, total) - view.start, 16);
    return view;
}

std::string splicedSelectionSize(const SampleSelection& selection, double sampleRate, bool inSamples)
{
    char text[64];
    if (inSamples)
        std::snprintf(text, sizeof text, "%zu samp", selection.length());
    else
        std::snprintf(text, sizeof text, "%.4f s", static_cast<double>(selection.length()) / sampleRate);
    return text;
}

std::string splicedSelectionRange(const SampleSelection& selection, double sampleRate, bool inSamples)
{
    char text[96];
    if (inSamples)
        std::snprintf(text, sizeof text, "%zu - %zu samp", selection.start, selection.end);
    else
        std::snprintf(text, sizeof text, "%.4f - %.4f s", static_cast<double>(selection.start) / sampleRate,
            static_cast<double>(selection.end) / sampleRate);
    return text;
}
}
