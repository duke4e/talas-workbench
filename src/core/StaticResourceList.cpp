#include "talas/core/StaticResourceList.h"

#include <algorithm>
#include <utility>

namespace talas::core
{
namespace
{
[[nodiscard]] bool isAsciiWhitespace(char character) noexcept
{
    return character == ' '
        || character == '\t'
        || character == '\n'
        || character == '\r'
        || character == '\f'
        || character == '\v';
}

void trimAsciiWhitespace(std::string_view& text) noexcept
{
    while (!text.empty() && isAsciiWhitespace(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && isAsciiWhitespace(text.back()))
        text.remove_suffix(1);
}
}

std::optional<std::vector<std::string>>
parseBoundedStaticResourceList(std::string_view text)
{
    if (text.empty() || text.size() > maximumStaticResourceListTextBytes)
        return std::nullopt;

    std::vector<std::string> resources;
    resources.reserve(std::min(
        maximumStaticResourceListPositions, text.size()));
    std::string current;
    const auto flush = [&] {
        if (current.empty())
            return true;
        if (resources.size() >= maximumStaticResourceListPositions)
            return false;
        resources.push_back(std::move(current));
        current.clear();
        return true;
    };
    for (std::size_t offset = 0; offset < text.size(); ++offset)
    {
        const auto character = text[offset];
        if (character == ',' || isAsciiWhitespace(character))
        {
            if (!flush())
                return std::nullopt;
        }
        else if (character == '\'' && current.empty())
        {
            // A name may be written within single quotes, so it may hold
            // spaces; a doubled quote within it is one quote, as in
            // Smalltalk.
            auto closed = false;
            for (++offset; offset < text.size(); ++offset)
            {
                if (text[offset] != '\'')
                {
                    current.push_back(text[offset]);
                    continue;
                }
                if (offset + 1 < text.size() && text[offset + 1] == '\'')
                {
                    current.push_back('\'');
                    ++offset;
                    continue;
                }
                closed = true;
                break;
            }
            if (!closed || current.empty()
                || (offset + 1 < text.size() && text[offset + 1] != ','
                    && !isAsciiWhitespace(text[offset + 1]))
                || !flush())
            {
                return std::nullopt;
            }
        }
        else
        {
            current.push_back(character);
        }
    }
    if (!flush() || resources.empty())
        return std::nullopt;
    return resources;
}

bool beginsStaticStringResourceSelection(std::string_view text) noexcept
{
    trimAsciiWhitespace(text);
    if (text.starts_with('('))
    {
        text.remove_prefix(1);
        trimAsciiWhitespace(text);
    }
    return text.starts_with("#(");
}

std::optional<std::string>
selectBoundedStaticStringResource(std::string_view text)
{
    if (text.size() > maximumStaticResourceListTextBytes)
        return std::nullopt;
    trimAsciiWhitespace(text);
    const bool wrapped = text.starts_with('(');
    if (wrapped)
    {
        text.remove_prefix(1);
        trimAsciiWhitespace(text);
    }
    if (!text.starts_with("#("))
        return std::nullopt;
    text.remove_prefix(2);

    std::vector<std::string> strings;
    while (true)
    {
        while (!text.empty() && isAsciiWhitespace(text.front()))
            text.remove_prefix(1);
        if (text.starts_with(')'))
        {
            text.remove_prefix(1);
            break;
        }
        if (!text.starts_with('\'')
            || strings.size() >= maximumStaticResourceListPositions)
        {
            return std::nullopt;
        }
        text.remove_prefix(1);
        std::string value;
        bool closed = false;
        while (!text.empty())
        {
            const char character = text.front();
            text.remove_prefix(1);
            if (character == '\'')
            {
                if (text.starts_with('\''))
                {
                    value.push_back('\'');
                    text.remove_prefix(1);
                    continue;
                }
                closed = true;
                break;
            }
            if (static_cast<unsigned char>(character) < 0x20U)
                return std::nullopt;
            value.push_back(character);
        }
        if (!closed || value.empty())
            return std::nullopt;
        strings.push_back(std::move(value));
        if (!text.empty() && !isAsciiWhitespace(text.front())
            && text.front() != ')')
        {
            return std::nullopt;
        }
    }
    if (strings.empty())
        return std::nullopt;
    trimAsciiWhitespace(text);
    if (!text.starts_with("at:"))
        return std::nullopt;
    text.remove_prefix(3);
    trimAsciiWhitespace(text);
    if (text.empty() || text.front() < '1' || text.front() > '9')
        return std::nullopt;
    std::size_t oneBasedIndex = 0;
    while (!text.empty() && text.front() >= '0' && text.front() <= '9')
    {
        oneBasedIndex = oneBasedIndex * 10U
            + static_cast<std::size_t>(text.front() - '0');
        if (oneBasedIndex > strings.size())
            return std::nullopt;
        text.remove_prefix(1);
    }
    trimAsciiWhitespace(text);
    if (wrapped)
    {
        if (!text.starts_with(')'))
            return std::nullopt;
        text.remove_prefix(1);
        trimAsciiWhitespace(text);
    }
    if (!text.empty() || oneBasedIndex == 0)
        return std::nullopt;
    return strings[oneBasedIndex - 1];
}

std::optional<std::vector<std::string>> splitQuotedWords(std::string_view line)
{
    std::vector<std::string> words;
    std::size_t offset = 0;
    while (offset < line.size())
    {
        if (isAsciiWhitespace(line[offset]))
        {
            ++offset;
            continue;
        }
        std::string word;
        if (line[offset] != '\'')
        {
            while (offset < line.size() && !isAsciiWhitespace(line[offset]))
                word.push_back(line[offset++]);
            words.push_back(std::move(word));
            continue;
        }
        auto closed = false;
        for (++offset; offset < line.size(); ++offset)
        {
            if (line[offset] != '\'')
            {
                word.push_back(line[offset]);
                continue;
            }
            if (offset + 1 < line.size() && line[offset + 1] == '\'')
            {
                word.push_back('\'');
                ++offset;
                continue;
            }
            closed = true;
            ++offset;
            break;
        }
        if (!closed || (offset < line.size() && !isAsciiWhitespace(line[offset])))
            return std::nullopt;
        words.push_back(std::move(word));
    }
    return words;
}

std::string quotedWord(std::string_view word)
{
    if (!word.empty() && std::ranges::none_of(word, [](char character) {
            return character == '\'' || isAsciiWhitespace(character);
        }))
    {
        return std::string{word};
    }
    std::string quoted{"'"};
    for (const auto character : word)
    {
        quoted.push_back(character);
        if (character == '\'')
            quoted.push_back('\'');
    }
    quoted.push_back('\'');
    return quoted;
}
}
