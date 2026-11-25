#include "talas/app/TextSearch.h"

#include <algorithm>

namespace talas::app
{
std::optional<TextMatch> findNextInText(std::string_view text, std::string_view needle, std::size_t from)
{
    if (needle.empty() || from > text.size())
        return std::nullopt;
    const auto found = text.find(needle, from);
    if (found == std::string_view::npos)
        return std::nullopt;
    return TextMatch{found, found + needle.size()};
}

TextReplacement replaceInText(
    std::string_view text, std::string_view needle, std::string_view replacement, std::size_t from, bool all)
{
    TextReplacement result{std::string{text}, 0, std::nullopt};
    if (needle.empty())
        return result;
    if (!all)
    {
        const auto found = findNextInText(text, needle, from);
        if (!found.has_value())
            return result;
        result.text.replace(found->start, needle.size(), replacement);
        result.count = 1;
        result.replaced = TextMatch{found->start, found->start + replacement.size()};
        return result;
    }
    std::string replaced;
    std::size_t position = 0;
    for (auto found = text.find(needle); found != std::string_view::npos; found = text.find(needle, position))
    {
        replaced.append(text.substr(position, found - position));
        replaced.append(replacement);
        position = found + needle.size();
        ++result.count;
    }
    replaced.append(text.substr(position));
    result.text = std::move(replaced);
    return result;
}
}
