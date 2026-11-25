#include "talas/app/ParameterFieldEditors.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <limits>

namespace talas::app
{
namespace
{
[[nodiscard]] std::string_view trimmed(std::string_view text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0)
        text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0)
        text.remove_suffix(1);
    return text;
}

[[nodiscard]] std::optional<double> numberIn(std::string_view text)
{
    text = trimmed(text);
    if (text.empty())
        return std::nullopt;
    auto value = 0.0;
    const auto* begin = text.data();
    if (*begin == '+')
        ++begin;
    const auto [end, failed] = std::from_chars(begin, text.data() + text.size(), value);
    if (failed != std::errc{} || end != text.data() + text.size() || !std::isfinite(value))
        return std::nullopt;
    return value;
}

// A number and the word after it, as `0.1 s`.
[[nodiscard]] std::optional<std::pair<double, std::string>> numberWithUnit(std::string_view text)
{
    text = trimmed(text);
    const auto space = text.find_first_of(" \t");
    if (space == std::string_view::npos)
    {
        const auto number = numberIn(text);
        if (!number)
            return std::nullopt;
        return std::pair{*number, std::string{}};
    }
    const auto number = numberIn(text.substr(0, space));
    const auto unit = trimmed(text.substr(space));
    if (!number || unit.empty()
        || !std::ranges::all_of(unit, [](char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; }))
        return std::nullopt;
    return std::pair{*number, std::string{unit}};
}

constexpr auto envelopeLimit = std::size_t{4096};
}

std::string fieldNumberText(double value)
{
    if (value == 0.0)
        value = 0.0;
    std::array<char, 64> buffer{};
    const auto [end, failed] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return failed == std::errc{} ? std::string(buffer.data(), end) : std::string{"0"};
}

std::optional<EnvelopeField> parseEnvelopeField(std::string_view text)
{
    EnvelopeField field;
    std::vector<std::string_view> expressions;
    std::optional<std::size_t> start;
    std::optional<std::size_t> end;
    std::size_t at = 0;
    while (at < text.size())
    {
        const auto character = text[at];
        if (std::isspace(static_cast<unsigned char>(character)) != 0)
        {
            ++at;
            continue;
        }
        if (character == '{')
        {
            const auto close = text.find('}', at + 1);
            if (close == std::string_view::npos)
                return std::nullopt;
            expressions.push_back(text.substr(at + 1, close - at - 1));
            at = close + 1;
            continue;
        }
        auto wordEnd = at;
        while (wordEnd < text.size() && std::isalnum(static_cast<unsigned char>(text[wordEnd])) != 0)
            ++wordEnd;
        const auto word = text.substr(at, wordEnd - at);
        if ((word != "loopStart" && word != "loopEnd") || expressions.size() < 2 || expressions.size() % 2 != 0)
            return std::nullopt;
        auto& slot = word == "loopStart" ? start : end;
        if (slot.has_value())
            return std::nullopt;
        slot = expressions.size() / 2 - 1;
        at = wordEnd;
    }
    if (expressions.size() < 4 || expressions.size() % 2 != 0 || expressions.size() / 2 > envelopeLimit)
        return std::nullopt;
    for (std::size_t index = 0; index < expressions.size(); index += 2)
    {
        const auto time = numberWithUnit(expressions[index]);
        const auto value = numberIn(expressions[index + 1]);
        if (!time || !value)
            return std::nullopt;
        auto seconds = time->first;
        if (time->second == "ms")
            seconds /= 1000.0;
        else if (time->second == "usec")
            seconds /= 1'000'000.0;
        else if (!time->second.empty() && time->second != "s")
            return std::nullopt;
        if (seconds < 0.0 || (!field.points.empty() && seconds < field.points.back().seconds))
            return std::nullopt;
        field.points.push_back({seconds, *value});
    }
    if (start.has_value() != end.has_value() || (start.has_value() && *start >= *end))
        return std::nullopt;
    field.loopStart = start;
    field.loopEnd = end;
    return field;
}

std::string envelopeFieldText(const EnvelopeField& field)
{
    std::string text;
    const auto looped = field.loopStart.has_value() && field.loopEnd.has_value() && *field.loopStart < *field.loopEnd;
    for (std::size_t index = 0; index < field.points.size(); ++index)
    {
        if (!text.empty())
            text += ' ';
        text += "{" + fieldNumberText(field.points[index].seconds) + " s} {"
            + fieldNumberText(field.points[index].value) + "}";
        if (looped && index == *field.loopStart)
            text += " loopStart";
        if (looped && index == *field.loopEnd)
            text += " loopEnd";
    }
    return text;
}

std::size_t addEnvelopeBreakpoint(EnvelopeField& field, double seconds, double value)
{
    seconds = std::max(0.0, std::isfinite(seconds) ? seconds : 0.0);
    value = std::clamp(std::isfinite(value) ? value : 0.0, 0.0, 1.0);
    const auto position = std::ranges::upper_bound(field.points, seconds, {}, &EnvelopeBreakpoint::seconds);
    const auto index = static_cast<std::size_t>(position - field.points.begin());
    field.points.insert(position, {seconds, value});
    for (auto* mark : {&field.loopStart, &field.loopEnd})
    {
        if (mark->has_value() && **mark >= index)
            ++**mark;
    }
    return index;
}

bool removeEnvelopeBreakpoint(EnvelopeField& field, std::size_t index)
{
    if (index >= field.points.size() || field.points.size() <= 2)
        return false;
    field.points.erase(field.points.begin() + static_cast<std::ptrdiff_t>(index));
    for (auto* mark : {&field.loopStart, &field.loopEnd})
    {
        if (!mark->has_value())
            continue;
        if (**mark == index)
            mark->reset();
        else if (**mark > index)
            --**mark;
    }
    return true;
}

void moveEnvelopeBreakpoint(EnvelopeField& field, std::size_t index, double seconds, double value)
{
    if (index >= field.points.size())
        return;
    const auto earliest = index == 0 ? 0.0 : field.points[index - 1].seconds;
    const auto latest = index + 1 < field.points.size() ? field.points[index + 1].seconds
                                                        : std::numeric_limits<double>::max();
    field.points[index].seconds = std::clamp(std::isfinite(seconds) ? seconds : earliest, earliest, latest);
    field.points[index].value = std::clamp(std::isfinite(value) ? value : 0.0, 0.0, 1.0);
}

std::uint32_t envelopeLoopArrowColour() noexcept
{
    return 0xffe0443cu;
}

bool toggleEnvelopeLoopStart(EnvelopeField& field, std::size_t index)
{
    if (index >= field.points.size())
        return false;
    if (field.loopStart == index)
    {
        field.loopStart.reset();
        return true;
    }
    if (field.loopEnd.has_value() && index >= *field.loopEnd)
        return false;
    field.loopStart = index;
    return true;
}

bool toggleEnvelopeLoopEnd(EnvelopeField& field, std::size_t index)
{
    if (index >= field.points.size())
        return false;
    if (field.loopEnd == index)
    {
        field.loopEnd.reset();
        return true;
    }
    if (field.loopStart.has_value() && index <= *field.loopStart)
        return false;
    field.loopEnd = index;
    return true;
}

std::optional<std::size_t> nearestEnvelopeBreakpoint(
    const EnvelopeField& field, double seconds, double value, double secondsReach, double valueReach)
{
    std::optional<std::size_t> nearest;
    auto best = std::numeric_limits<double>::infinity();
    for (std::size_t index = 0; index < field.points.size(); ++index)
    {
        const auto dx = (field.points[index].seconds - seconds) / std::max(secondsReach, 1e-12);
        const auto dy = (field.points[index].value - value) / std::max(valueReach, 1e-12);
        if (std::abs(dx) > 1.0 || std::abs(dy) > 1.0)
            continue;
        const auto distance = dx * dx + dy * dy;
        if (distance < best)
        {
            best = distance;
            nearest = index;
        }
    }
    return nearest;
}

std::optional<std::vector<ArrayFieldElement>> parseArrayField(std::string_view text)
{
    std::vector<ArrayFieldElement> elements;
    std::size_t at = 0;
    const auto skipSpace = [&] {
        while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at])) != 0)
            ++at;
    };
    while (true)
    {
        skipSpace();
        if (at >= text.size())
            break;
        if (elements.size() >= 1024)
            return std::nullopt;
        if (text[at] == '{')
        {
            const auto close = text.find('}', at + 1);
            if (close == std::string_view::npos)
                return std::nullopt;
            const auto inside = numberWithUnit(text.substr(at + 1, close - at - 1));
            if (!inside)
                return std::nullopt;
            elements.push_back({inside->first, inside->second});
            at = close + 1;
            continue;
        }
        auto tokenEnd = at;
        while (tokenEnd < text.size() && std::isspace(static_cast<unsigned char>(text[tokenEnd])) == 0)
            ++tokenEnd;
        const auto number = numberIn(text.substr(at, tokenEnd - at));
        if (!number)
            return std::nullopt;
        at = tokenEnd;
        ArrayFieldElement element{*number, {}};
        // A unit word after the number belongs to it.
        auto next = at;
        while (next < text.size() && std::isspace(static_cast<unsigned char>(text[next])) != 0)
            ++next;
        auto wordEnd = next;
        while (wordEnd < text.size() && std::isalpha(static_cast<unsigned char>(text[wordEnd])) != 0)
            ++wordEnd;
        if (wordEnd > next && (wordEnd == text.size() || std::isspace(static_cast<unsigned char>(text[wordEnd])) != 0))
        {
            element.unit = std::string{text.substr(next, wordEnd - next)};
            at = wordEnd;
        }
        elements.push_back(std::move(element));
    }
    if (elements.empty())
        return std::nullopt;
    return elements;
}

std::string arrayFieldText(const std::vector<ArrayFieldElement>& elements)
{
    std::string text;
    for (const auto& element : elements)
    {
        if (!text.empty())
            text += ' ';
        text += fieldNumberText(element.value);
        if (!element.unit.empty())
            text += " " + element.unit;
    }
    return text;
}

ArrayFieldRange arrayFieldRange(const std::vector<ArrayFieldElement>& elements)
{
    ArrayFieldRange range;
    for (const auto& element : elements)
    {
        range.minimum = std::min(range.minimum, element.value);
        range.maximum = std::max(range.maximum, element.value);
    }
    return range;
}

namespace
{
[[nodiscard]] bool startsWithIgnoringCase(std::string_view name, std::string_view prefix)
{
    if (prefix.size() > name.size())
        return false;
    for (std::size_t index = 0; index < prefix.size(); ++index)
    {
        if (std::tolower(static_cast<unsigned char>(name[index])) != std::tolower(static_cast<unsigned char>(prefix[index])))
            return false;
    }
    return true;
}

[[nodiscard]] bool isNameCharacter(char character)
{
    return std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_';
}
}

const std::vector<std::string>& commonHotValueNames()
{
    static const std::vector<std::string> names{"Volume", "Frequency", "Gain", "Rate", "Pan", "Morph", "BPM",
        "Interval", "Scale", "Level", "KeyPitch", "KeyDown", "KeyVelocity", "KeyNumber", "KeyTimbre", "PitchBend",
        "Fader", "Gate", "Trigger", "TimeIndex"};
    return names;
}

std::string fieldHotValueName(std::string_view parameterId)
{
    std::string name{parameterId};
    if (!name.empty())
        name.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(name.front())));
    return name;
}

std::optional<std::string> hotValueCompletion(
    std::string_view typed, std::string_view fieldName, const std::vector<std::string>& projectNames)
{
    if (!std::ranges::all_of(typed, isNameCharacter))
        return std::nullopt;
    const auto field = fieldHotValueName(fieldName);
    if (typed.empty())
        return field.empty() ? std::nullopt : std::optional<std::string>{field};
    std::vector<std::string> candidates{field};
    auto project = projectNames;
    std::ranges::sort(project);
    candidates.insert(candidates.end(), project.begin(), project.end());
    const auto& common = commonHotValueNames();
    candidates.insert(candidates.end(), common.begin(), common.end());
    for (const auto& candidate : candidates)
    {
        if (candidate.size() > typed.size() && startsWithIgnoringCase(candidate, typed))
            return candidate;
    }
    return std::nullopt;
}

std::optional<std::string> hotValueBeingTyped(std::string_view text, std::size_t caret)
{
    if (caret > text.size() || (caret < text.size() && isNameCharacter(text[caret])))
        return std::nullopt;
    auto start = caret;
    while (start > 0 && isNameCharacter(text[start - 1]))
        --start;
    if (start == 0 || text[start - 1] != '!')
        return std::nullopt;
    return std::string{text.substr(start, caret - start)};
}

std::pair<std::size_t, std::size_t> doubleClickSelection(std::string_view text, std::size_t caret)
{
    caret = std::min(caret, text.size());
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
        return {0, text.size()};
    const auto last = text.find_last_not_of(" \t\r\n") + 1;
    if (caret <= first || caret >= last)
        return {0, text.size()};

    constexpr std::string_view openers = "([{";
    constexpr std::string_view closers = ")]}";
    // What a bracket just before the caret opens, up to its match.
    if (const auto open = openers.find(text[caret - 1]); open != std::string_view::npos)
    {
        auto depth = 0;
        for (auto index = caret; index < text.size(); ++index)
        {
            if (text[index] == openers[open])
                ++depth;
            else if (text[index] == closers[open] && depth-- == 0)
                return {caret, index};
        }
    }
    // What a bracket just after the caret closes, back to its match.
    if (const auto close = closers.find(text[caret]); close != std::string_view::npos)
    {
        auto depth = 0;
        for (auto index = caret; index-- > 0;)
        {
            if (text[index] == closers[close])
                ++depth;
            else if (text[index] == openers[close] && depth-- == 0)
                return {index + 1, caret};
        }
    }
    // A quote opens when an even number of its kind come before it.
    const auto quotesBefore = [&](std::size_t position, char quote) {
        return static_cast<std::size_t>(std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(position), quote));
    };
    for (const auto quote : {'\'', '"'})
    {
        if (text[caret - 1] == quote && quotesBefore(caret - 1, quote) % 2 == 0)
        {
            if (const auto end = text.find(quote, caret); end != std::string_view::npos)
                return {caret, end};
        }
        if (text[caret] == quote && quotesBefore(caret, quote) % 2 == 1)
        {
            if (const auto start = text.rfind(quote, caret - 1); start != std::string_view::npos)
                return {start + 1, caret};
        }
    }
    // The word or number at the caret.
    const auto wordCharacter = [&](std::size_t index) {
        const auto character = static_cast<unsigned char>(text[index]);
        return std::isalnum(character) != 0 || character == '_';
    };
    const auto digit = [&](std::size_t index) {
        return index < text.size() && std::isdigit(static_cast<unsigned char>(text[index])) != 0;
    };
    auto start = caret;
    auto end = caret;
    for (;;)
    {
        if (start > 0 && wordCharacter(start - 1))
            --start;
        else if (start > 1 && text[start - 1] == '.' && digit(start - 2) && digit(start))
            --start;
        else
            break;
    }
    for (;;)
    {
        if (end < text.size() && wordCharacter(end))
            ++end;
        else if (end + 1 < text.size() && text[end] == '.' && end > 0 && digit(end - 1) && digit(end + 1))
            ++end;
        else
            break;
    }
    if (start > 0 && text[start - 1] == '!' && start < end)
        --start;
    return {start, end};
}

std::string pastedSoundText(std::string_view soundName)
{
    return std::string{soundName} + " L";
}

std::vector<FieldSoundSpan> fieldSoundSpans(std::string_view text, const std::vector<std::string>& sounds)
{
    const auto word = [](char character) {
        return std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_';
    };
    std::vector<FieldSoundSpan> spans;
    std::size_t index = 0;
    while (index < text.size())
    {
        const auto character = text[index];
        // A string or a comment, to its closing mark.
        if (character == '\'' || character == '"')
        {
            const auto close = text.find(character, index + 1);
            index = close == std::string_view::npos ? text.size() : close + 1;
            continue;
        }
        if (!word(character))
        {
            ++index;
            continue;
        }
        const auto start = index;
        while (index < text.size() && word(text[index]))
            ++index;
        const auto marked = start > 0
            && (text[start - 1] == '!' || text[start - 1] == '?' || text[start - 1] == '`' || text[start - 1] == '#');
        const auto name = text.substr(start, index - start);
        if (marked || std::isdigit(static_cast<unsigned char>(name.front())) != 0
            || std::ranges::find(sounds, name) == sounds.end())
            continue;
        auto end = index;
        // Its channel: L, R or M as a word of its own after it.
        if (end + 1 < text.size() && text[end] == ' ' && (text[end + 1] == 'L' || text[end + 1] == 'R' || text[end + 1] == 'M')
            && (end + 2 == text.size() || !word(text[end + 2])))
            end += 2;
        spans.push_back({start, end});
        index = end;
    }
    return spans;
}

std::vector<FieldTextSpan> fieldTextSpans(std::string_view text)
{
    std::vector<FieldTextSpan> spans;
    const auto add = [&](std::string_view part, FieldTextKind kind) {
        if (part.empty())
            return;
        if (!spans.empty() && spans.back().kind == kind && kind == FieldTextKind::Plain)
            spans.back().text += part;
        else
            spans.push_back({std::string{part}, kind});
    };
    const auto nameCharacter = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
    std::size_t at = 0;
    while (at < text.size())
    {
        const auto c = text[at];
        // A quoted string or a comment runs to its closing quote.
        if (c == '\'' || c == '"')
        {
            const auto close = text.find(c, at + 1);
            const auto end = close == std::string_view::npos ? text.size() : close + 1;
            add(text.substr(at, end - at), FieldTextKind::Plain);
            at = end;
            continue;
        }
        const auto kind = c == '!' ? FieldTextKind::EventValue
            : c == '?'             ? FieldTextKind::Variable
            : c == '`'             ? FieldTextKind::EventSource
                                   : FieldTextKind::Plain;
        if (kind != FieldTextKind::Plain && at + 1 < text.size() && std::isalpha(static_cast<unsigned char>(text[at + 1])))
        {
            auto end = at + 1;
            while (end < text.size() && nameCharacter(text[end]))
                ++end;
            add(text.substr(at, end - at), kind);
            at = end;
            continue;
        }
        add(text.substr(at, 1), FieldTextKind::Plain);
        ++at;
    }
    return spans;
}

std::uint32_t fieldTextColour(FieldTextKind kind) noexcept
{
    switch (kind)
    {
    case FieldTextKind::EventValue: return 0xffe04848u;
    case FieldTextKind::Variable: return 0xff3db54au;
    case FieldTextKind::EventSource: return 0xff4f86e8u;
    case FieldTextKind::Plain: return 0;
    }
    return 0;
}

std::vector<std::string> inputDropChoices(const std::vector<std::string>& sounds, bool acceptsMany)
{
    if (!acceptsMany || sounds.empty())
        return {};
    std::vector<std::string> choices{"Add to the Sounds"};
    for (const auto& sound : sounds)
        choices.push_back("Replace " + sound);
    return choices;
}

bool parametersMayMoveTo(bool illegalValueHeld, bool sameSound) noexcept
{
    return !illegalValueHeld || sameSound;
}

bool typedFieldValueTested(FieldTestMoment moment) noexcept
{
    return moment == FieldTestMoment::DifferentSoundClicked || moment == FieldTestMoment::ActionMenuOperation
        || moment == FieldTestMoment::InfoMenuOperation;
}

bool fieldSelectsAllWhenFocused(bool inputField) noexcept
{
    return !inputField;
}

std::optional<std::size_t> tabbedParameterField(std::size_t count, std::size_t current, bool forward) noexcept
{
    if (current >= count)
        return std::nullopt;
    if (forward)
        return current + 1 < count ? std::optional<std::size_t>{current + 1} : std::nullopt;
    return current > 0 ? std::optional<std::size_t>{current - 1} : std::nullopt;
}

void PasteHistory::remember(std::string text)
{
    if (text.empty())
        return;
    std::erase(texts_, text);
    texts_.insert(texts_.begin(), std::move(text));
    if (texts_.size() > limit)
        texts_.resize(limit);
}

std::vector<EnvelopeGridLine> envelopeGridLines(double visibleSeconds, double rate)
{
    if (!std::isfinite(rate) || rate <= 0.0)
        rate = 1.0;
    std::vector<EnvelopeGridLine> lines;
    if (!std::isfinite(visibleSeconds) || visibleSeconds <= 0.0)
        return lines;
    const auto tenths = static_cast<int>(std::min(400.0, std::ceil(visibleSeconds * 10.0)));
    for (int tenth = 1; tenth < tenths; ++tenth)
    {
        const auto seconds = tenth / 10.0;
        lines.push_back({seconds, seconds / rate, tenth % 10 == 0});
    }
    return lines;
}

double envelopeFieldRate(std::string_view rateText)
{
    const auto rate = numberIn(rateText);
    return rate.has_value() && *rate > 0.0 ? *rate : 1.0;
}

std::optional<std::string> parameterValueOfSound(const talas::core::NodeTypeDescriptor& sound, std::string_view parameterId)
{
    const auto parameter = std::ranges::find(sound.parameters, parameterId, &talas::core::ParameterDescriptor::id);
    if (parameter == sound.parameters.end() || parameter->kind != talas::core::ParameterKind::Expression)
        return std::nullopt;
    return parameter->defaultExpression;
}
IllegalFieldSignal illegalFieldSignal()
{
    return {{0, 180}, {0, 120, 240, 360}};
}
}
