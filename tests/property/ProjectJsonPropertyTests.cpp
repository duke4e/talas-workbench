#include "talas/project/ProjectJson.h"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace
{
constexpr std::uint64_t propertySeed = 0x4a534f4e50524f50ULL;

[[nodiscard]] std::string caseContext(std::size_t caseIndex)
{
    return " seed=" + std::to_string(propertySeed)
        + " case=" + std::to_string(caseIndex);
}

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

[[nodiscard]] std::uint64_t nextRandom(std::uint64_t& state) noexcept
{
    state ^= state << 13U;
    state ^= state >> 7U;
    state ^= state << 17U;
    return state;
}

[[nodiscard]] std::string generatedTitle(std::uint64_t& state)
{
    constexpr auto characters = std::string_view{
        " abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789\n\t\"\\"};
    const auto length = static_cast<std::size_t>(nextRandom(state) % 33U);
    std::string title;
    title.reserve(length);
    for (std::size_t index = 0; index < length; ++index)
    {
        title.push_back(characters[static_cast<std::size_t>(
            nextRandom(state) % characters.size())]);
    }
    return title;
}

[[nodiscard]] bool requireCanonicalRoundTrip(
    std::string_view source,
    std::size_t caseIndex)
{
    const auto parsed = talas::project::parseProjectDocument(source);
    if (!parsed.ok())
        return false;
    const auto canonical =
        talas::project::serializeProjectDocument(parsed.document);
    const auto reopened = talas::project::parseProjectDocument(canonical);
    require(
        reopened.ok(),
        "project property reopen failed" + caseContext(caseIndex));
    require(
        talas::project::serializeProjectDocument(reopened.document)
            == canonical,
        "project property canonical bytes changed" + caseContext(caseIndex));
    return true;
}

void mutate(
    std::string& source,
    std::uint64_t& state)
{
    if (source.empty())
    {
        source.push_back(static_cast<char>(nextRandom(state)));
        return;
    }
    const auto index = static_cast<std::size_t>(
        nextRandom(state) % source.size());
    switch (nextRandom(state) % 4U)
    {
    case 0:
        source[index] ^= static_cast<char>(
            1U << (nextRandom(state) % 7U));
        break;
    case 1:
        source.insert(
            source.begin() + static_cast<std::ptrdiff_t>(index),
            static_cast<char>(nextRandom(state)));
        break;
    case 2:
        source.erase(
            source.begin() + static_cast<std::ptrdiff_t>(index));
        break;
    default:
        source.resize(index);
        break;
    }
}
}

int main()
{
    auto random = propertySeed;
    for (std::size_t caseIndex = 0; caseIndex < 256; ++caseIndex)
    {
        talas::project::ProjectDocument document;
        document.metadata.title = generatedTitle(random);
        document.timeline.loop.enabled = (nextRandom(random) & 1U) != 0;
        document.timeline.loop.startSeconds =
            static_cast<double>(nextRandom(random) % 100U) / 10.0;
        document.timeline.loop.endSeconds =
            document.timeline.loop.startSeconds
            + 0.1
            + static_cast<double>(nextRandom(random) % 100U) / 10.0;
        const auto canonical =
            talas::project::serializeProjectDocument(document);
        require(
            requireCanonicalRoundTrip(canonical, caseIndex),
            "generated canonical project did not parse"
                + caseContext(caseIndex));

        auto mutated = canonical;
        const auto mutationCount = 1U + static_cast<std::size_t>(
            nextRandom(random) % 8U);
        for (std::size_t mutation = 0;
             mutation < mutationCount;
             ++mutation)
        {
            mutate(mutated, random);
        }
        (void)requireCanonicalRoundTrip(mutated, caseIndex);
    }
}
