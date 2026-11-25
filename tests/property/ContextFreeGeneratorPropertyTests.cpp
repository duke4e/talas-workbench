#include "talas/generative/ContextFreeGenerator.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

[[nodiscard]] std::uint64_t referenceNextRandomWord(
    std::uint64_t& state) noexcept
{
    state += 0x9e3779b97f4a7c15ULL;
    auto word = state;
    word = (word ^ (word >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    word = (word ^ (word >> 27U)) * 0x94d049bb133111ebULL;
    return word ^ (word >> 31U);
}

[[nodiscard]] std::uint64_t undoXorShiftRight(
    std::uint64_t value,
    unsigned shift) noexcept
{
    auto result = value;
    for (auto next = shift; next < 64U; next *= 2U)
        result ^= result >> next;
    return result;
}

[[nodiscard]] std::uint64_t seedForFirstWord(
    std::uint64_t word) noexcept
{
    auto state = undoXorShiftRight(word, 31U);
    state *= 0x319642b2d24d8ec3ULL;
    state = undoXorShiftRight(state, 27U);
    state *= 0x96de1b173f119089ULL;
    state = undoXorShiftRight(state, 30U);
    return state - 0x9e3779b97f4a7c15ULL;
}

[[nodiscard]] std::optional<std::size_t> referenceDraw(
    std::uint64_t& state,
    std::size_t bound)
{
    if (bound == 0)
        return std::nullopt;
    const auto divisor = static_cast<std::uint64_t>(bound);
    const auto threshold = (0ULL - divisor) % divisor;
    for (std::size_t draw = 0;
         draw < talas::generative::maximumContextFreeOperations;
         ++draw)
    {
        const auto word = referenceNextRandomWord(state);
        if (word >= threshold)
            return static_cast<std::size_t>(word % divisor);
    }
    return std::nullopt;
}

struct ReferenceResult
{
    bool ok = false;
    std::vector<std::string> symbols;
};

ReferenceResult referenceGenerate(std::uint64_t seed)
{
    std::vector<std::string> current{"A", "B"};
    for (std::size_t pass = 0; pass < 16; ++pass)
    {
        bool complete = true;
        for (const auto& symbol : current)
            complete = complete && symbol != "A" && symbol != "B";
        if (complete)
            return {true, std::move(current)};
        std::vector<std::string> next;
        for (const auto& symbol : current)
        {
            if (symbol == "A")
            {
                const auto choice = referenceDraw(seed, 3);
                if (!choice.has_value())
                    return {};
                if (*choice == 0)
                {
                    next.push_back("x");
                    next.push_back("A");
                }
                else if (*choice == 1)
                {
                    next.push_back("y");
                    next.push_back("A");
                }
                else
                {
                    next.push_back("z");
                }
            }
            else if (symbol == "B")
            {
                const auto choice = referenceDraw(seed, 3);
                if (!choice.has_value())
                    return {};
                if (*choice == 0)
                {
                    next.push_back("z");
                    next.push_back("B");
                }
                else if (*choice == 1)
                {
                    next.push_back("x");
                    next.push_back("B");
                }
            }
            else
            {
                next.push_back(symbol);
            }
            if (next.size() > 64)
                return {};
        }
        current = std::move(next);
    }
    const auto complete = std::ranges::none_of(
        current,
        [](const auto& symbol) { return symbol == "A" || symbol == "B"; });
    return {complete, complete ? std::move(current) : std::vector<std::string>{}};
}

void testPreparedExecutorMatchesIndependentParallelOracle()
{
    const auto verifySeed = [](std::uint64_t seed) {
        const talas::generative::ContextFreeGeneratorSource source{
            {"x", "y", "z"},
            "start: A B\n"
            "A -> x A | y A | z\n"
            "B -> z B | x B | epsilon\n",
            seed,
            16,
            64,
        };
        const auto prepared = talas::generative::prepareContextFreeGenerator(source);
        require(prepared.ok(), "property grammar should prepare");
        std::array<std::uint16_t, 64> current{};
        std::array<std::uint16_t, 64> next{};
        std::array<std::uint16_t, 64> output{};
        const talas::generative::ContextFreeGeneratorScratch scratch{
            current, next, output};
        const auto actual = talas::generative::generateContextFreeSymbols(
            prepared.program, scratch);
        const auto expected = referenceGenerate(seed);
        require(actual.ok() == expected.ok,
            "prepared generator status should match parallel oracle");
        if (!actual.ok())
            return;
        const auto indices = talas::generative::generatedContextFreeSymbolIndices(
            scratch, actual);
        require(indices.size() == expected.symbols.size(),
            "prepared generator output count should match oracle");
        for (std::size_t index = 0; index < indices.size(); ++index)
        {
            require(indices[index] < prepared.program.symbols.size()
                    && prepared.program.symbols[indices[index]]
                        == expected.symbols[index],
                "prepared generator terminal order should match oracle");
        }
    };
    for (std::uint64_t seed = 0; seed < 4096; ++seed)
        verifySeed(seed);
    const auto rejectionSeed = seedForFirstWord(0);
    std::uint64_t rejectionState = rejectionSeed;
    require(referenceNextRandomWord(rejectionState) == 0,
        "independent inverse fixture should force one rejected draw");
    verifySeed(rejectionSeed);
}

void testBoundedParserMutationReplay()
{
    constexpr std::string_view alphabet =
        "ABCxyz012_ -:>|\n\r\t#[]{}";
    std::uint64_t state = 0x6b635f6366675f31ULL;
    for (std::size_t caseIndex = 0; caseIndex < 4096; ++caseIndex)
    {
        const auto length = static_cast<std::size_t>(
            talas::generative::contextFreeNextRandomWord(state) % 129U);
        std::string grammar;
        grammar.reserve(length);
        for (std::size_t index = 0; index < length; ++index)
        {
            grammar.push_back(alphabet[
                talas::generative::contextFreeNextRandomWord(state)
                % alphabet.size()]);
        }
        const talas::generative::ContextFreeGeneratorSource source{
            {"x", "y"}, grammar, caseIndex, 8, 64};
        const auto parsed = talas::generative::parseContextFreeGenerator(source);
        require(parsed.diagnostics.size()
                <= talas::generative::maximumContextFreeDiagnostics,
            "mutated parser diagnostics must remain bounded");
        if (!parsed.ok())
            continue;
        const auto prepared = talas::generative::prepareContextFreeGenerator(source);
        require(prepared.ok(),
            "every accepted parse should prepare under the same authority");
        std::array<std::uint16_t, 64> current{};
        std::array<std::uint16_t, 64> next{};
        std::array<std::uint16_t, 64> output{};
        const auto generated = talas::generative::generateContextFreeSymbols(
            prepared.program, {current, next, output});
        require(generated.outputCount <= 64,
            "mutated accepted program output must remain within its bound");
    }
}
}

int main()
{
    try
    {
        testPreparedExecutorMatchesIndependentParallelOracle();
        testBoundedParserMutationReplay();
    }
    catch (const std::exception& exception)
    {
        std::cerr << "ContextFreeGeneratorPropertyTests failed: "
                  << exception.what() << '\n';
        return 1;
    }
    return 0;
}
