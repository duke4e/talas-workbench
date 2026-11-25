#include "SemanticVersion.h"

#include <array>
#include <ranges>

namespace talas::project
{
namespace
{
[[nodiscard]] bool validIdentifier(
    std::string_view identifier,
    bool forbidNumericLeadingZero)
{
    if (identifier.empty())
        return false;
    const auto asciiDigit = [](char character) {
        return character >= '0' && character <= '9';
    };
    const auto numeric = std::ranges::all_of(identifier, asciiDigit);
    if (forbidNumericLeadingZero && numeric
        && identifier.size() > 1 && identifier.front() == '0')
    {
        return false;
    }
    return std::ranges::all_of(
        identifier,
        [asciiDigit](char character) {
            return asciiDigit(character)
                || (character >= 'a' && character <= 'z')
                || (character >= 'A' && character <= 'Z')
                || character == '-';
        });
}

[[nodiscard]] bool validIdentifiers(
    std::string_view value,
    bool forbidNumericLeadingZero)
{
    std::size_t start = 0;
    while (start <= value.size())
    {
        const auto separator = value.find('.', start);
        const auto identifier = value.substr(
            start,
            separator == std::string_view::npos
                ? value.size() - start
                : separator - start);
        if (!validIdentifier(identifier, forbidNumericLeadingZero))
            return false;
        if (separator == std::string_view::npos)
            return true;
        start = separator + 1;
    }
    return false;
}

struct ParsedSemanticVersion
{
    std::array<std::string_view, 3> core;
    std::string_view prerelease;
};

[[nodiscard]] ParsedSemanticVersion parseValidVersion(
    std::string_view version) noexcept
{
    const auto buildSeparator = version.find('+');
    const auto withoutBuild = version.substr(0, buildSeparator);
    const auto prereleaseSeparator = withoutBuild.find('-');
    const auto core = withoutBuild.substr(0, prereleaseSeparator);

    ParsedSemanticVersion parsed;
    parsed.prerelease = prereleaseSeparator == std::string_view::npos
        ? std::string_view{}
        : withoutBuild.substr(prereleaseSeparator + 1U);
    std::size_t start = 0;
    for (auto& identifier : parsed.core)
    {
        const auto separator = core.find('.', start);
        identifier = core.substr(
            start,
            separator == std::string_view::npos
                ? core.size() - start
                : separator - start);
        start = separator == std::string_view::npos
            ? core.size()
            : separator + 1U;
    }
    return parsed;
}

[[nodiscard]] int compareNumericIdentifiers(
    std::string_view left,
    std::string_view right) noexcept
{
    if (left.size() != right.size())
        return left.size() < right.size() ? -1 : 1;
    if (left == right)
        return 0;
    return left < right ? -1 : 1;
}

[[nodiscard]] bool numericIdentifier(std::string_view value) noexcept
{
    return std::ranges::all_of(value, [](char character) {
        return character >= '0' && character <= '9';
    });
}

[[nodiscard]] std::string_view nextIdentifier(
    std::string_view value,
    std::size_t& offset) noexcept
{
    if (offset > value.size())
        return {};
    const auto separator = value.find('.', offset);
    const auto identifier = value.substr(
        offset,
        separator == std::string_view::npos
            ? value.size() - offset
            : separator - offset);
    offset = separator == std::string_view::npos
        ? value.size() + 1U
        : separator + 1U;
    return identifier;
}
}

bool isCanonicalSemanticVersion(
    std::string_view version,
    std::size_t maximumBytes) noexcept
{
    if (version.empty() || version.size() > maximumBytes)
        return false;

    const auto buildSeparator = version.find('+');
    const auto build = buildSeparator == std::string_view::npos
        ? std::string_view{}
        : version.substr(buildSeparator + 1);
    auto withoutBuild = buildSeparator == std::string_view::npos
        ? version
        : version.substr(0, buildSeparator);
    if (buildSeparator != std::string_view::npos
        && (build.empty() || build.find('+') != std::string_view::npos))
    {
        return false;
    }

    const auto prereleaseSeparator = withoutBuild.find('-');
    const auto prerelease = prereleaseSeparator == std::string_view::npos
        ? std::string_view{}
        : withoutBuild.substr(prereleaseSeparator + 1);
    const auto core = prereleaseSeparator == std::string_view::npos
        ? withoutBuild
        : withoutBuild.substr(0, prereleaseSeparator);
    if (prereleaseSeparator != std::string_view::npos && prerelease.empty())
        return false;

    std::array<std::string_view, 3> coreIdentifiers;
    std::size_t start = 0;
    for (std::size_t index = 0; index < coreIdentifiers.size(); ++index)
    {
        const auto separator = core.find('.', start);
        if ((index + 1 < coreIdentifiers.size()
             && separator == std::string_view::npos)
            || (index + 1 == coreIdentifiers.size()
                && separator != std::string_view::npos))
        {
            return false;
        }
        coreIdentifiers[index] = core.substr(
            start,
            separator == std::string_view::npos
                ? core.size() - start
                : separator - start);
        start = separator == std::string_view::npos
            ? core.size()
            : separator + 1;
    }
    for (const auto identifier : coreIdentifiers)
    {
        if (!validIdentifier(identifier, true)
            || !std::ranges::all_of(
                identifier,
                [](char character) {
                    return character >= '0' && character <= '9';
                }))
        {
            return false;
        }
    }
    if (!prerelease.empty() && !validIdentifiers(prerelease, true))
        return false;
    return build.empty() || validIdentifiers(build, false);
}

SemanticVersionOrder compareCanonicalSemanticVersionPrecedence(
    std::string_view left,
    std::string_view right,
    std::size_t maximumBytes) noexcept
{
    if (!isCanonicalSemanticVersion(left, maximumBytes)
        || !isCanonicalSemanticVersion(right, maximumBytes))
    {
        return SemanticVersionOrder::Invalid;
    }

    const auto parsedLeft = parseValidVersion(left);
    const auto parsedRight = parseValidVersion(right);
    for (std::size_t index = 0; index < parsedLeft.core.size(); ++index)
    {
        const auto order = compareNumericIdentifiers(
            parsedLeft.core[index],
            parsedRight.core[index]);
        if (order < 0)
            return SemanticVersionOrder::Less;
        if (order > 0)
            return SemanticVersionOrder::Greater;
    }

    if (parsedLeft.prerelease.empty() || parsedRight.prerelease.empty())
    {
        if (parsedLeft.prerelease.empty()
            && parsedRight.prerelease.empty())
        {
            return SemanticVersionOrder::Equivalent;
        }
        return parsedLeft.prerelease.empty()
            ? SemanticVersionOrder::Greater
            : SemanticVersionOrder::Less;
    }

    std::size_t leftOffset = 0;
    std::size_t rightOffset = 0;
    while (leftOffset <= parsedLeft.prerelease.size()
           && rightOffset <= parsedRight.prerelease.size())
    {
        const auto leftIdentifier =
            nextIdentifier(parsedLeft.prerelease, leftOffset);
        const auto rightIdentifier =
            nextIdentifier(parsedRight.prerelease, rightOffset);
        const auto leftNumeric = numericIdentifier(leftIdentifier);
        const auto rightNumeric = numericIdentifier(rightIdentifier);
        if (leftNumeric != rightNumeric)
        {
            return leftNumeric
                ? SemanticVersionOrder::Less
                : SemanticVersionOrder::Greater;
        }
        const auto order = leftNumeric
            ? compareNumericIdentifiers(leftIdentifier, rightIdentifier)
            : leftIdentifier == rightIdentifier
                ? 0
                : leftIdentifier < rightIdentifier ? -1 : 1;
        if (order < 0)
            return SemanticVersionOrder::Less;
        if (order > 0)
            return SemanticVersionOrder::Greater;
    }
    if (leftOffset > parsedLeft.prerelease.size()
        && rightOffset > parsedRight.prerelease.size())
    {
        return SemanticVersionOrder::Equivalent;
    }
    return leftOffset > parsedLeft.prerelease.size()
        ? SemanticVersionOrder::Less
        : SemanticVersionOrder::Greater;
}
}
