#include "talas/app/SampleEditorModel.h"

#include "talas/expr/Expression.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <system_error>

namespace talas::app
{
namespace
{
constexpr std::size_t undoLimit = 32;

[[nodiscard]] std::string_view trimmed(std::string_view text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0)
        text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0)
        text.remove_suffix(1);
    return text;
}

[[nodiscard]] std::optional<double> plainNumber(std::string_view text)
{
    text = trimmed(text);
    if (text.empty())
        return std::nullopt;
    if (text.front() == '+')
        text.remove_prefix(1);
    auto value = 0.0;
    const auto [end, failed] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (failed != std::errc{} || end != text.data() + text.size() || !std::isfinite(value))
        return std::nullopt;
    return value;
}

// A value in curly braces, as {2 * Float pi}, evaluated as
// Val.
[[nodiscard]] std::optional<double> braceValue(std::string_view text)
{
    const auto parsed = talas::expr::parse(text, talas::expr::ExpressionLanguageVersion::ValV1);
    if (!parsed.ok() || !parsed.expression->eventReferences().empty() || !parsed.expression->identifiers().empty()
        || parsed.expression->usesTime())
        return std::nullopt;
    const auto evaluated = talas::expr::evaluate(*parsed.expression, {}, {});
    if (!evaluated.ok() || !std::isfinite(evaluated.value.value))
        return std::nullopt;
    return evaluated.value.value;
}

[[nodiscard]] std::uint32_t nextRandom(std::uint32_t& state)
{
    state = static_cast<std::uint32_t>((static_cast<std::uint64_t>(state) * 16807u) % 2147483647u);
    return state;
}

// Natural cubic spline through (x, y) knots, evaluated at x.
[[nodiscard]] double splineAt(const std::vector<double>& xs, const std::vector<double>& ys,
    const std::vector<double>& second, double x)
{
    std::size_t segment = 0;
    while (segment + 2 < xs.size() && x > xs[segment + 1])
        ++segment;
    const auto h = xs[segment + 1] - xs[segment];
    if (h <= 0.0)
        return ys[segment];
    const auto a = (xs[segment + 1] - x) / h;
    const auto b = (x - xs[segment]) / h;
    return a * ys[segment] + b * ys[segment + 1]
        + ((a * a * a - a) * second[segment] + (b * b * b - b) * second[segment + 1]) * h * h / 6.0;
}

[[nodiscard]] std::vector<double> splineSecondDerivatives(const std::vector<double>& xs, const std::vector<double>& ys)
{
    const auto n = xs.size();
    std::vector<double> second(n, 0.0);
    std::vector<double> u(n, 0.0);
    for (std::size_t i = 1; i + 1 < n; ++i)
    {
        const auto sig = (xs[i] - xs[i - 1]) / (xs[i + 1] - xs[i - 1]);
        const auto p = sig * second[i - 1] + 2.0;
        second[i] = (sig - 1.0) / p;
        const auto slope = (ys[i + 1] - ys[i]) / (xs[i + 1] - xs[i]) - (ys[i] - ys[i - 1]) / (xs[i] - xs[i - 1]);
        u[i] = (6.0 * slope / (xs[i + 1] - xs[i - 1]) - sig * u[i - 1]) / p;
    }
    second[n - 1] = 0.0;
    for (std::size_t i = n - 1; i-- > 0;)
        second[i] = second[i] * second[i + 1] + u[i];
    return second;
}

// Segments between end points, each its proportion of the duration.
template <typename Shape>
std::optional<std::vector<double>> segments(std::size_t frames, const std::vector<double>& points,
    const std::vector<double>& proportions, const Shape& shape, std::string& error)
{
    if (points.size() < 2 || proportions.size() + 1 != points.size())
    {
        error = "The segments need one proportion fewer than their end points";
        return std::nullopt;
    }
    auto total = 0.0;
    for (const auto proportion : proportions)
    {
        if (!(proportion >= 0.0))
        {
            error = "A segment's proportion is not negative";
            return std::nullopt;
        }
        total += proportion;
    }
    if (!(total > 0.0))
    {
        error = "The segments' proportions add up to more than 0";
        return std::nullopt;
    }
    std::vector<double> segment(frames, points.back());
    auto begin = 0.0;
    for (std::size_t index = 0; index < proportions.size(); ++index)
    {
        const auto length = proportions[index] / total * static_cast<double>(frames);
        const auto first = static_cast<std::size_t>(std::llround(begin));
        const auto last = std::min(frames, static_cast<std::size_t>(std::llround(begin + length)));
        for (auto frame = first; frame < last; ++frame)
        {
            const auto t = length > 0.0 ? (static_cast<double>(frame) - begin) / length : 0.0;
            segment[frame] = shape(points[index], points[index + 1], std::clamp(t, 0.0, 1.0));
        }
        begin += length;
    }
    return segment;
}

[[nodiscard]] double sinc(double x)
{
    return x == 0.0 ? 1.0 : std::sin(std::numbers::pi * x) / (std::numbers::pi * x);
}
}

SampleEditorModel::SampleEditorModel(double sampleRate, std::size_t channels, std::size_t frames)
    : sampleRate_(sampleRate > 0.0 && std::isfinite(sampleRate) ? sampleRate : 48000.0)
    , channels_(std::clamp<std::size_t>(channels, 1, maximumSampleEditorChannels),
          std::vector<double>(std::min(frames, maximumSampleEditorFrames), 0.0))
{
}

std::optional<SampleEditorModel> SampleEditorModel::fromChannels(double sampleRate, std::vector<std::vector<double>> channels)
{
    if (channels.empty() || channels.size() > maximumSampleEditorChannels || !(sampleRate > 0.0)
        || channels.front().size() > maximumSampleEditorFrames
        || std::ranges::any_of(channels, [&](const auto& channel) { return channel.size() != channels.front().size(); }))
        return std::nullopt;
    SampleEditorModel model(sampleRate, channels.size(), 0);
    model.channels_ = std::move(channels);
    return model;
}

void SampleEditorModel::select(std::size_t start, std::size_t end)
{
    if (start > end)
        std::swap(start, end);
    selection_ = {std::min(start, frames()), std::min(end, frames())};
}

void SampleEditorModel::selectAll()
{
    selection_ = {0, frames()};
}

void SampleEditorModel::remember()
{
    undo_.push_back({channels_, selection_});
    if (undo_.size() > undoLimit)
        undo_.erase(undo_.begin());
    redo_.clear();
    modified_ = true;
}

std::filesystem::path pastedSampleClipPath(const std::filesystem::path& folder, const std::filesystem::path& source)
{
    const auto stem = source.empty() ? std::string{"Sample"} : source.stem().string();
    for (std::size_t number = 1;; ++number)
    {
        auto candidate = folder / (stem + " clip " + std::to_string(number) + ".wav");
        std::error_code error;
        if (!std::filesystem::exists(candidate, error))
            return candidate;
    }
}

bool SampleEditorModel::copy()
{
    if (selection_.empty())
        return false;
    clipboard_.clear();
    for (const auto& channel : channels_)
        clipboard_.emplace_back(channel.begin() + static_cast<std::ptrdiff_t>(selection_.start),
            channel.begin() + static_cast<std::ptrdiff_t>(selection_.end));
    return true;
}

bool SampleEditorModel::removeRange(std::size_t start, std::size_t end)
{
    if (end <= start)
        return false;
    remember();
    for (auto& channel : channels_)
        channel.erase(channel.begin() + static_cast<std::ptrdiff_t>(start), channel.begin() + static_cast<std::ptrdiff_t>(end));
    selection_ = {start, start};
    return true;
}

bool SampleEditorModel::cut()
{
    return copy() && removeRange(selection_.start, selection_.end);
}

bool SampleEditorModel::clear()
{
    return removeRange(selection_.start, selection_.end);
}

bool SampleEditorModel::trim()
{
    if (selection_.empty() || selection_.length() == frames())
        return false;
    remember();
    for (auto& channel : channels_)
        channel = std::vector<double>(channel.begin() + static_cast<std::ptrdiff_t>(selection_.start),
            channel.begin() + static_cast<std::ptrdiff_t>(selection_.end));
    selection_ = {0, frames()};
    return true;
}

bool SampleEditorModel::replaceSelection(const std::vector<std::vector<double>>& segments)
{
    if (segments.empty())
        return false;
    const auto length = segments.front().size();
    if (std::ranges::any_of(segments, [&](const auto& segment) { return segment.size() != length; })
        || frames() - selection_.length() + length > maximumSampleEditorFrames)
        return false;
    remember();
    for (std::size_t index = 0; index < channels_.size(); ++index)
    {
        // A mono segment goes on every channel; a stereo one's left on a
        // mono file.
        const auto& segment = segments[std::min(index, segments.size() - 1)];
        auto& channel = channels_[index];
        channel.erase(channel.begin() + static_cast<std::ptrdiff_t>(selection_.start),
            channel.begin() + static_cast<std::ptrdiff_t>(selection_.end));
        channel.insert(channel.begin() + static_cast<std::ptrdiff_t>(selection_.start), segment.begin(), segment.end());
    }
    selection_ = {selection_.start, selection_.start + length};
    return true;
}

bool SampleEditorModel::paste()
{
    return hasClipboard() && replaceSelection(clipboard_);
}

bool SampleEditorModel::insertSegment(const std::vector<double>& segment)
{
    return !segment.empty() && replaceSelection({segment});
}

void SampleEditorModel::beginStroke()
{
    remember();
}

bool SampleEditorModel::drawLine(std::optional<std::size_t> channel, std::size_t fromFrame, double fromValue,
    std::size_t toFrame, double toValue)
{
    if (frames() == 0 || (channel.has_value() && *channel >= channels_.size()))
        return false;
    fromFrame = std::min(fromFrame, frames() - 1);
    toFrame = std::min(toFrame, frames() - 1);
    if (fromFrame > toFrame)
    {
        std::swap(fromFrame, toFrame);
        std::swap(fromValue, toValue);
    }
    fromValue = std::clamp(fromValue, -1.0, 1.0);
    toValue = std::clamp(toValue, -1.0, 1.0);
    modified_ = true;
    for (auto frame = fromFrame; frame <= toFrame; ++frame)
    {
        const auto t = toFrame == fromFrame ? 1.0 : static_cast<double>(frame - fromFrame) / static_cast<double>(toFrame - fromFrame);
        const auto value = fromValue + (toValue - fromValue) * t;
        for (std::size_t index = 0; index < channels_.size(); ++index)
        {
            if (!channel.has_value() || *channel == index)
                channels_[index][frame] = value;
        }
    }
    return true;
}

bool SampleEditorModel::undo()
{
    if (undo_.empty())
        return false;
    redo_.push_back({channels_, selection_});
    channels_ = std::move(undo_.back().channels);
    selection_ = undo_.back().selection;
    undo_.pop_back();
    modified_ = true;
    return true;
}

bool SampleEditorModel::redo()
{
    if (redo_.empty())
        return false;
    undo_.push_back({channels_, selection_});
    channels_ = std::move(redo_.back().channels);
    selection_ = redo_.back().selection;
    redo_.pop_back();
    modified_ = true;
    return true;
}

double SampleEditorModel::peak() const
{
    const auto start = selection_.empty() ? 0 : selection_.start;
    const auto end = selection_.empty() ? frames() : selection_.end;
    auto peak = 0.0;
    for (const auto& channel : channels_)
    {
        for (auto frame = start; frame < end; ++frame)
            peak = std::max(peak, std::abs(channel[frame]));
    }
    return peak;
}

bool SampleEditorModel::setHeader(double sampleRate, std::size_t channels)
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0 || channels == 0 || channels > maximumSampleEditorChannels)
        return false;
    if (sampleRate == sampleRate_ && channels == channels_.size())
        return false;
    if (channels != channels_.size())
    {
        remember();
        if (channels < channels_.size())
            channels_.resize(channels);
        else
        {
            const auto left = channels_.front();
            channels_.resize(channels, left);
        }
    }
    sampleRate_ = sampleRate;
    modified_ = true;
    return true;
}

std::string sampleSelectionReadout(const SampleEditorModel& editor, bool inSamples)
{
    const auto& selection = editor.selection();
    char text[160];
    if (inSamples)
    {
        std::snprintf(text, sizeof text, "%zu - %zu (%zu samples)", selection.start, selection.end, selection.length());
    }
    else
    {
        const auto rate = editor.sampleRate();
        std::snprintf(text, sizeof text, "%.4f s - %.4f s (%.4f s)", static_cast<double>(selection.start) / rate,
            static_cast<double>(selection.end) / rate, static_cast<double>(selection.length()) / rate);
    }
    return text;
}

std::string samplePeakReadout(double peak, bool inDecibels)
{
    char text[64];
    if (!inDecibels)
        std::snprintf(text, sizeof text, "%.4f", peak);
    else if (peak <= 0.0)
        std::snprintf(text, sizeof text, "-inf dB");
    else
        std::snprintf(text, sizeof text, "%.1f dB", 20.0 * std::log10(peak));
    return text;
}

std::string sampleMagnificationReadout(std::size_t viewFrames, int pixels)
{
    char text[64];
    const auto perPixel = static_cast<double>(viewFrames) / static_cast<double>(std::max(1, pixels));
    if (perPixel >= 1.0)
        std::snprintf(text, sizeof text, "1:%.0f", std::round(perPixel));
    else
        std::snprintf(text, sizeof text, "%.0f:1", std::round(1.0 / std::max(perPixel, 1.0e-9)));
    return text;
}

std::vector<std::vector<double>> sampleEditorPlayed(const SampleEditorModel& editor)
{
    const auto& selection = editor.selection();
    const auto start = selection.empty() ? 0 : selection.start;
    const auto end = selection.empty() ? editor.frames() : selection.end;
    std::vector<std::vector<double>> played;
    for (const auto& channel : editor.channels())
        played.emplace_back(channel.begin() + static_cast<std::ptrdiff_t>(start), channel.begin() + static_cast<std::ptrdiff_t>(end));
    return played;
}

std::vector<std::pair<double, double>> sampleLissajous(const SampleEditorModel& editor, std::size_t delay, std::size_t limit)
{
    std::vector<std::pair<double, double>> points;
    if (editor.channels().empty() || limit == 0)
        return points;
    const auto& samples = editor.channels().front();
    const auto& selection = editor.selection();
    const auto start = selection.empty() ? 0 : selection.start;
    const auto end = selection.empty() ? samples.size() : selection.end;
    if (end <= start + delay)
        return points;
    const auto count = end - start - delay;
    const auto step = std::max<std::size_t>(1, count / limit);
    for (auto frame = start; frame + delay < end && points.size() < limit; frame += step)
        points.emplace_back(samples[frame], samples[frame + delay]);
    return points;
}

SampleEditorLayout sampleEditorLayout(int width, int height) noexcept
{
    // A margin of 8 all round; 150 for the templates and 28 for the buttons
    // from the bottom; the overview 60 high, 6 above the detail view.
    auto x = 8;
    auto y = 8;
    const auto inner = std::max(0, width - 16);
    auto remaining = std::max(0, height - 16);
    const auto take = [&](int wanted) {
        const auto taken = std::min(wanted, remaining);
        remaining -= taken;
        return taken;
    };
    SampleEditorLayout layout;
    const auto templates = take(150);
    layout.templates = {x, y + remaining, inner, templates};
    const auto toolbar = take(28);
    layout.toolbar = {x, y + remaining, inner, toolbar};
    layout.overview = {x, y, inner, take(60)};
    y += layout.overview.height;
    y += take(6);
    layout.detail = {x, y, inner, remaining};
    return layout;
}

std::vector<std::vector<double>> fullWaveformLanes(std::vector<double> left, std::vector<double> right)
{
    if (right.empty())
        right = left;
    return {std::move(left), std::move(right)};
}

WaveformExtremes waveformExtremes(const std::vector<std::vector<double>>& channels)
{
    WaveformExtremes extremes;
    bool any = false;
    for (const auto& channel : channels)
    {
        for (const auto sample : channel)
        {
            extremes.minimum = any ? std::min(extremes.minimum, sample) : sample;
            extremes.maximum = any ? std::max(extremes.maximum, sample) : sample;
            any = true;
        }
    }
    return extremes;
}

std::string_view sampleTemplateName(SampleTemplate kind)
{
    switch (kind)
    {
    case SampleTemplate::Buzz: return "Buzz";
    case SampleTemplate::CubicSpline: return "Cubic Spline";
    case SampleTemplate::ExponentialSegments: return "Exponential Segments";
    case SampleTemplate::Fourier: return "Fourier";
    case SampleTemplate::ImpulseResponse: return "Impulse Response";
    case SampleTemplate::InterpolateSelection: return "Interpolate Selection";
    case SampleTemplate::LineSegments: return "Line Segments";
    case SampleTemplate::NormalizeSelection: return "Normalize Selection";
    case SampleTemplate::Polynomial: return "Polynomial";
    case SampleTemplate::Random: return "Random";
    case SampleTemplate::WindowSelection: return "Window Selection";
    }
    return {};
}

const std::vector<SampleTemplate>& sampleTemplates()
{
    static const std::vector<SampleTemplate> all{SampleTemplate::Buzz, SampleTemplate::CubicSpline,
        SampleTemplate::ExponentialSegments, SampleTemplate::Fourier, SampleTemplate::ImpulseResponse,
        SampleTemplate::InterpolateSelection, SampleTemplate::LineSegments, SampleTemplate::NormalizeSelection,
        SampleTemplate::Polynomial, SampleTemplate::Random, SampleTemplate::WindowSelection};
    return all;
}

std::vector<SampleTemplateField> sampleTemplateFields(SampleTemplate kind)
{
    switch (kind)
    {
    case SampleTemplate::Buzz:
        return {{"Duration", "4096 samp"}, {"Harmonics", "8"}, {"Lowest Harmonic", "1"}, {"AmpBase", "0.8"},
            {"Gain", "0.5"}};
    case SampleTemplate::CubicSpline:
        return {{"Duration", "4096 samp"}, {"Points", "0 1 0 -1 0"}, {"Proportions", "0.25 0.25 0.25 0.25"}};
    case SampleTemplate::ExponentialSegments:
        return {{"Duration", "4096 samp"}, {"End Points", "0.001 1 0.001"}, {"Proportions", "0.1 0.9"}};
    case SampleTemplate::Fourier:
        return {{"Duration", "4096 samp"}, {"Harmonics", "1 3 5"}, {"Amplitudes", "1 0.333 0.2"},
            {"Phases", "0 0 0"}};
    case SampleTemplate::ImpulseResponse:
        return {{"Duration", "4096 samp"}, {"Frequencies", "500 1500 2500"}, {"Bandwidths", "80 120 160"},
            {"Amplitudes", "1 0.5 0.25"}, {"Gain", "0.1"}};
    case SampleTemplate::InterpolateSelection:
        return {{"Length", "4096 samp"}, {"Gain", "1"}, {"Interpolation Points", "8"}};
    case SampleTemplate::LineSegments:
        return {{"Duration", "4096 samp"}, {"End Points", "0 1 0"}, {"Proportions", "0.5 0.5"}};
    case SampleTemplate::NormalizeSelection:
        return {{"Amplitude", "1"}};
    case SampleTemplate::Polynomial:
        return {{"Duration", "4096 samp"}, {"Coefficients", "0 1"}, {"Interval", "-1 1"}};
    case SampleTemplate::Random:
        return {{"Duration", "4096 samp"}, {"Seed", "1"}};
    case SampleTemplate::WindowSelection:
        return {{"Transition Duration", "0.01 s"}};
    }
    return {};
}

std::optional<std::size_t> sampleTemplateFrames(std::string_view text, double sampleRate)
{
    text = trimmed(text);
    const auto space = text.find_first_of(" \t");
    const auto number = plainNumber(text.substr(0, space));
    const auto unit = space == std::string_view::npos ? std::string_view{} : trimmed(text.substr(space));
    if (!number || *number < 0.0)
        return std::nullopt;
    auto frames = *number;
    if (unit == "s")
        frames *= sampleRate;
    else if (unit == "ms")
        frames *= sampleRate / 1000.0;
    else if (!unit.empty() && unit != "samp")
        return std::nullopt;
    if (frames > static_cast<double>(maximumSampleEditorFrames))
        return std::nullopt;
    return static_cast<std::size_t>(std::llround(frames));
}

std::optional<std::vector<double>> sampleTemplateNumbers(std::string_view text)
{
    std::vector<double> numbers;
    std::size_t at = 0;
    while (at < text.size())
    {
        if (std::isspace(static_cast<unsigned char>(text[at])) != 0)
        {
            ++at;
            continue;
        }
        if (numbers.size() >= 4096)
            return std::nullopt;
        if (text[at] == '{')
        {
            const auto close = text.find('}', at + 1);
            if (close == std::string_view::npos)
                return std::nullopt;
            const auto value = braceValue(text.substr(at + 1, close - at - 1));
            if (!value)
                return std::nullopt;
            numbers.push_back(*value);
            at = close + 1;
            continue;
        }
        auto end = at;
        while (end < text.size() && std::isspace(static_cast<unsigned char>(text[end])) == 0)
            ++end;
        const auto value = plainNumber(text.substr(at, end - at));
        if (!value)
            return std::nullopt;
        numbers.push_back(*value);
        at = end;
    }
    return numbers;
}

SampleTemplateResult applySampleTemplate(
    SampleEditorModel& editor, SampleTemplate kind, const std::map<std::string, std::string>& fields)
{
    SampleTemplateResult result;
    const auto field = [&](const std::string& name) {
        if (const auto found = fields.find(name); found != fields.end())
            return found->second;
        for (const auto& given : sampleTemplateFields(kind))
        {
            if (given.name == name)
                return given.defaultText;
        }
        return std::string{};
    };
    const auto numbers = [&](const std::string& name) -> std::optional<std::vector<double>> {
        auto parsed = sampleTemplateNumbers(field(name));
        if (!parsed || parsed->empty())
            result.error = name + " takes numbers separated by spaces";
        return parsed && !parsed->empty() ? parsed : std::nullopt;
    };
    const auto number = [&](const std::string& name) -> std::optional<double> {
        const auto parsed = numbers(name);
        if (!parsed)
            return std::nullopt;
        if (parsed->size() != 1)
        {
            result.error = name + " takes one number";
            return std::nullopt;
        }
        return parsed->front();
    };
    const auto duration = [&](const std::string& name) -> std::optional<std::size_t> {
        const auto frames = sampleTemplateFrames(field(name), editor.sampleRate());
        if (!frames || *frames == 0)
        {
            result.error = name + " is a duration of samples, as 4096 samp or 0.1 s";
            return std::nullopt;
        }
        return frames;
    };
    const auto insert = [&](std::optional<std::vector<double>> segment) {
        if (segment && !editor.insertSegment(*segment) && result.error.empty())
            result.error = "The segment does not fit in the sample file";
        return result;
    };
    const auto needSelection = [&] {
        if (editor.selection().empty())
            result.error = std::string{sampleTemplateName(kind)} + " works on a selection";
        return !editor.selection().empty();
    };
    switch (kind)
    {
    case SampleTemplate::Buzz:
    {
        // Harmonics sines from the Lowest Harmonic, each AmpBase
        // raised to its harmonic number, one cycle over the duration.
        const auto frames = duration("Duration");
        const auto harmonics = frames ? number("Harmonics") : std::nullopt;
        const auto lowest = harmonics ? number("Lowest Harmonic") : std::nullopt;
        const auto base = lowest ? number("AmpBase") : std::nullopt;
        const auto gain = base ? number("Gain") : std::nullopt;
        if (!gain)
            return result;
        if (*harmonics < 1.0 || *harmonics > 4096.0 || *lowest < 1.0)
        {
            result.error = "Buzz takes 1 to 4096 harmonics from harmonic 1 up";
            return result;
        }
        std::vector<double> segment(*frames, 0.0);
        for (std::size_t k = 0; k < static_cast<std::size_t>(*harmonics); ++k)
        {
            const auto harmonic = std::floor(*lowest) + static_cast<double>(k);
            const auto amplitude = std::pow(*base, harmonic) * *gain;
            for (std::size_t n = 0; n < *frames; ++n)
                segment[n] += amplitude
                    * std::sin(2.0 * std::numbers::pi * harmonic * static_cast<double>(n) / static_cast<double>(*frames));
        }
        return insert(segment);
    }
    case SampleTemplate::Fourier:
    {
        // Harmonics with their amplitudes and phases in radians.
        const auto frames = duration("Duration");
        const auto harmonics = frames ? numbers("Harmonics") : std::nullopt;
        const auto amplitudes = harmonics ? numbers("Amplitudes") : std::nullopt;
        const auto phases = amplitudes ? numbers("Phases") : std::nullopt;
        if (!phases)
            return result;
        if (amplitudes->size() != harmonics->size() || phases->size() != harmonics->size())
        {
            result.error = "Fourier takes an amplitude and a phase for each harmonic";
            return result;
        }
        std::vector<double> segment(*frames, 0.0);
        for (std::size_t k = 0; k < harmonics->size(); ++k)
        {
            for (std::size_t n = 0; n < *frames; ++n)
                segment[n] += (*amplitudes)[k]
                    * std::sin(2.0 * std::numbers::pi * (*harmonics)[k] * static_cast<double>(n)
                            / static_cast<double>(*frames)
                        + (*phases)[k]);
        }
        return insert(segment);
    }
    case SampleTemplate::LineSegments:
    case SampleTemplate::ExponentialSegments:
    case SampleTemplate::CubicSpline:
    {
        const auto exponential = kind == SampleTemplate::ExponentialSegments;
        const auto frames = duration("Duration");
        const auto points = frames ? numbers(kind == SampleTemplate::CubicSpline ? "Points" : "End Points") : std::nullopt;
        const auto proportions = points ? numbers("Proportions") : std::nullopt;
        if (!proportions)
            return result;
        if (exponential && std::ranges::any_of(*points, [](double value) { return !(value > 0.0); }))
        {
            result.error = "exponential segments are undefined at 0, so their end points lie above 0";
            return result;
        }
        if (kind == SampleTemplate::CubicSpline)
        {
            if (points->size() < 2 || proportions->size() + 1 != points->size())
            {
                result.error = "The segments need one proportion fewer than their end points";
                return result;
            }
            auto total = 0.0;
            for (const auto proportion : *proportions)
                total += std::max(0.0, proportion);
            std::vector<double> xs{0.0};
            for (const auto proportion : *proportions)
                xs.push_back(xs.back() + std::max(0.0, proportion) / std::max(total, 1e-12));
            for (std::size_t i = 1; i < xs.size(); ++i)
            {
                if (!(xs[i] > xs[i - 1]))
                {
                    result.error = "Each spline segment takes some of the duration";
                    return result;
                }
            }
            const auto second = splineSecondDerivatives(xs, *points);
            std::vector<double> segment(*frames);
            for (std::size_t n = 0; n < *frames; ++n)
                segment[n] = splineAt(xs, *points, second,
                    *frames > 1 ? static_cast<double>(n) / static_cast<double>(*frames - 1) : 0.0);
            return insert(segment);
        }
        return insert(segments(*frames, *points, *proportions,
            [exponential](double from, double to, double t) {
                return exponential ? from * std::pow(to / from, t) : from + (to - from) * t;
            },
            result.error));
    }
    case SampleTemplate::ImpulseResponse:
    {
        // A filter's impulse response: each formant a decaying sine of its
        // centre frequency, decaying as its bandwidth says.
        const auto frames = duration("Duration");
        const auto frequencies = frames ? numbers("Frequencies") : std::nullopt;
        const auto bandwidths = frequencies ? numbers("Bandwidths") : std::nullopt;
        const auto amplitudes = bandwidths ? numbers("Amplitudes") : std::nullopt;
        const auto gain = amplitudes ? number("Gain") : std::nullopt;
        if (!gain)
            return result;
        if (bandwidths->size() != frequencies->size() || amplitudes->size() != frequencies->size())
        {
            result.error = "Impulse Response takes a bandwidth and an amplitude for each formant";
            return result;
        }
        std::vector<double> segment(*frames, 0.0);
        for (std::size_t k = 0; k < frequencies->size(); ++k)
        {
            for (std::size_t n = 0; n < *frames; ++n)
            {
                const auto t = static_cast<double>(n) / editor.sampleRate();
                segment[n] += *gain * (*amplitudes)[k] * std::exp(-std::numbers::pi * (*bandwidths)[k] * t)
                    * std::sin(2.0 * std::numbers::pi * (*frequencies)[k] * t);
            }
        }
        return insert(segment);
    }
    case SampleTemplate::Polynomial:
    {
        // The polynomial of the coefficients (the constant first)
        // over the interval's x values.
        const auto frames = duration("Duration");
        const auto coefficients = frames ? numbers("Coefficients") : std::nullopt;
        const auto interval = coefficients ? numbers("Interval") : std::nullopt;
        if (!interval)
            return result;
        if (interval->size() != 2)
        {
            result.error = "Interval takes the first and last x";
            return result;
        }
        std::vector<double> segment(*frames);
        for (std::size_t n = 0; n < *frames; ++n)
        {
            const auto x = (*interval)[0]
                + ((*interval)[1] - (*interval)[0])
                    * (*frames > 1 ? static_cast<double>(n) / static_cast<double>(*frames - 1) : 0.0);
            auto value = 0.0;
            for (auto coefficient = coefficients->rbegin(); coefficient != coefficients->rend(); ++coefficient)
                value = value * x + *coefficient;
            segment[n] = value;
        }
        return insert(segment);
    }
    case SampleTemplate::Random:
    {
        // Values from -1 to 1, repeatable for a seed (Park and
        // Miller's generator).
        const auto frames = duration("Duration");
        const auto seed = frames ? number("Seed") : std::nullopt;
        if (!seed)
            return result;
        if (*seed < 0.0 || *seed > 2147483645.0 || *seed != std::floor(*seed))
        {
            result.error = "A random seed is a whole number that is not negative";
            return result;
        }
        auto state = static_cast<std::uint32_t>(*seed) + 1u;
        std::vector<double> segment(*frames);
        for (auto& value : segment)
            value = static_cast<double>(nextRandom(state) - 1u) / 2147483646.0 * 2.0 - 1.0;
        return insert(segment);
    }
    case SampleTemplate::NormalizeSelection:
    {
        // The selection scaled so its peak is the Amplitude.
        const auto amplitude = needSelection() ? number("Amplitude") : std::nullopt;
        if (!amplitude)
            return result;
        const auto peak = editor.peak();
        if (!(peak > 0.0))
        {
            result.error = "The selection is silent";
            return result;
        }
        std::vector<std::vector<double>> scaled;
        for (const auto& channel : editor.channels())
        {
            std::vector<double> part(channel.begin() + static_cast<std::ptrdiff_t>(editor.selection().start),
                channel.begin() + static_cast<std::ptrdiff_t>(editor.selection().end));
            for (auto& value : part)
                value *= *amplitude / peak;
            scaled.push_back(std::move(part));
        }
        (void)editor.replaceSelection(scaled);
        return result;
    }
    case SampleTemplate::WindowSelection:
    {
        // A fade in and a fade out of the Transition Duration.
        const auto transition = needSelection() ? duration("Transition Duration") : std::nullopt;
        if (!transition)
            return result;
        const auto length = editor.selection().length();
        const auto fade = std::min(*transition, length / 2);
        std::vector<std::vector<double>> windowed;
        for (const auto& channel : editor.channels())
        {
            std::vector<double> part(channel.begin() + static_cast<std::ptrdiff_t>(editor.selection().start),
                channel.begin() + static_cast<std::ptrdiff_t>(editor.selection().end));
            for (std::size_t n = 0; n < fade; ++n)
            {
                const auto gain = 0.5 - 0.5 * std::cos(std::numbers::pi * static_cast<double>(n) / static_cast<double>(fade));
                part[n] *= gain;
                part[length - 1 - n] *= gain;
            }
            windowed.push_back(std::move(part));
        }
        (void)editor.replaceSelection(windowed);
        return result;
    }
    case SampleTemplate::InterpolateSelection:
    {
        // The selection sinc-interpolated to the Length, scaled by
        // Gain, over Interpolation Points on each side (Hann-windowed).
        const auto length = needSelection() ? duration("Length") : std::nullopt;
        const auto gain = length ? number("Gain") : std::nullopt;
        const auto points = gain ? number("Interpolation Points") : std::nullopt;
        if (!points)
            return result;
        if (*points < 1.0 || *points > 256.0)
        {
            result.error = "Interpolation Points are 1 to 256";
            return result;
        }
        const auto taps = static_cast<std::ptrdiff_t>(*points);
        const auto source = editor.selection().length();
        const auto ratio = static_cast<double>(source) / static_cast<double>(*length);
        // Shrinking lowers the cutoff, so nothing aliases.
        const auto cutoff = std::min(1.0, 1.0 / ratio);
        std::vector<std::vector<double>> resampled;
        for (const auto& channel : editor.channels())
        {
            std::vector<double> part(*length, 0.0);
            for (std::size_t n = 0; n < *length; ++n)
            {
                const auto position = static_cast<double>(n) * ratio;
                const auto centre = static_cast<std::ptrdiff_t>(std::floor(position));
                auto sum = 0.0;
                const auto reach = static_cast<std::ptrdiff_t>(std::ceil(static_cast<double>(taps) / cutoff));
                for (auto k = centre - reach + 1; k <= centre + reach; ++k)
                {
                    if (k < 0 || k >= static_cast<std::ptrdiff_t>(source))
                        continue;
                    const auto distance = position - static_cast<double>(k);
                    const auto window = std::abs(distance) * cutoff >= static_cast<double>(taps)
                        ? 0.0
                        : 0.5 + 0.5 * std::cos(std::numbers::pi * distance * cutoff / static_cast<double>(taps));
                    sum += channel[editor.selection().start + static_cast<std::size_t>(k)] * cutoff
                        * sinc(distance * cutoff) * window;
                }
                part[n] = sum * *gain;
            }
            resampled.push_back(std::move(part));
        }
        if (!editor.replaceSelection(resampled))
            result.error = "The interpolated selection does not fit in the sample file";
        return result;
    }
    }
    return result;
}
SampleEditorView sampleViewCentredAt(SampleEditorView view, std::size_t frame) noexcept
{
    view.start = frame > view.frames / 2 ? frame - view.frames / 2 : 0;
    return view;
}

SampleEditorView sampleViewPaged(SampleEditorView view, std::size_t total, bool right, std::size_t selectionFrames) noexcept
{
    const auto page = selectionFrames > 0 ? selectionFrames : view.frames;
    view.start = right ? std::min(view.start + page, total) : (view.start > page ? view.start - page : 0);
    return view;
}

SampleEditorView sampleViewFilled(SampleEditorView view, const SampleSelection& selection, double peak) noexcept
{
    if (selection.empty())
        return view;
    view.start = selection.start;
    view.frames = selection.length();
    if (peak > 0.0 && std::isfinite(peak))
        view.level = std::clamp(1.0 / peak, 0.125, 1024.0);
    return view;
}

SampleEditorView sampleViewZoomed(SampleEditorView view, std::size_t total, bool in) noexcept
{
    view.frames = std::clamp<std::size_t>(in ? view.frames / 2 : view.frames * 2, 16, std::max<std::size_t>(total, 64));
    return view;
}

bool sampleInsertionPointLit(std::uint64_t milliseconds) noexcept
{
    return milliseconds / 500 % 2 == 0;
}

std::uint32_t sampleInsertionPointColour() noexcept
{
    return 0xffe0443cu;
}

std::size_t sampleEditorShownChannels(std::size_t fileChannels) noexcept
{
    return std::max<std::size_t>(fileChannels, 2);
}

SampleEditorView sampleViewOfWholeFile(std::size_t frames) noexcept
{
    return {0, std::max<std::size_t>(frames, 64), 1.0};
}

SampleEditorView sampleViewAfterInsert(SampleEditorView view, std::size_t frames) noexcept
{
    if (view.frames < 64 || frames > view.start + view.frames)
        view.frames = std::max<std::size_t>(frames > view.start ? frames - view.start : 0, 64);
    return view;
}

SampleEditorView sampleViewLevelled(SampleEditorView view, bool in) noexcept
{
    view.level = std::clamp(in ? view.level * 2.0 : view.level / 2.0, 0.125, 1024.0);
    return view;
}

std::vector<SampleFileType> sampleFileTypes()
{
    std::vector<SampleFileType> types{
        SampleFileType::Wav, SampleFileType::Aiff, SampleFileType::Ircam, SampleFileType::SoundDesignerI};
#if defined(__APPLE__)
    types.push_back(SampleFileType::SoundDesignerII);
#endif
    return types;
}

std::string sampleFileTypeName(SampleFileType type)
{
    switch (type)
    {
    case SampleFileType::Wav: return "WAV";
    case SampleFileType::Aiff: return "AIFF";
    case SampleFileType::Ircam: return "IRCAM";
    case SampleFileType::SoundDesignerI: return "SD-I";
    case SampleFileType::SoundDesignerII: return "SD-II";
    }
    return "WAV";
}

SampleFileType sampleFileTypeOfExtension(const std::filesystem::path& path)
{
    auto extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    if (extension == ".aif" || extension == ".aiff" || extension == ".aifc")
        return SampleFileType::Aiff;
    if (extension == ".sf")
        return SampleFileType::Ircam;
    if (extension == ".sd1")
        return SampleFileType::SoundDesignerI;
    if (extension == ".sd2")
        return SampleFileType::SoundDesignerII;
    return SampleFileType::Wav;
}

std::string sampleFileExtension(SampleFileType type)
{
    switch (type)
    {
    case SampleFileType::Wav: return ".wav";
    case SampleFileType::Aiff: return ".aif";
    case SampleFileType::Ircam: return ".sf";
    case SampleFileType::SoundDesignerI: return ".sd1";
    case SampleFileType::SoundDesignerII: return ".sd2";
    }
    return ".wav";
}

std::string sampleFileFormatText(const SampleFileFormat& format)
{
    return sampleFileTypeName(format.type) + ", " + std::to_string(format.bits) + " bits";
}

std::optional<SampleFileFormat> sampleFileFormatFromInfo(SampleFileType type, std::uint16_t bits, std::size_t channels) noexcept
{
    if ((bits != 8 && bits != 16 && bits != 24) || (channels != 1 && channels != 2)
        || (type == SampleFileType::SoundDesignerI && channels != 1))
    {
        return std::nullopt;
    }
    return SampleFileFormat{type, channels, bits};
}

std::vector<std::vector<double>> sampleChannelsToSave(const std::vector<std::vector<double>>& channels, std::size_t count)
{
    if (channels.empty())
        return {};
    std::vector<std::vector<double>> saved;
    for (std::size_t channel = 0; channel < std::clamp<std::size_t>(count, 1, maximumSampleEditorChannels); ++channel)
        saved.push_back(channels[std::min(channel, channels.size() - 1)]);
    return saved;
}
}
