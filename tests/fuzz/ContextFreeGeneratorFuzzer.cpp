#include "talas/generative/ContextFreeGenerator.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string>
#include <vector>

namespace
{
[[noreturn]] void invariantFailure() noexcept
{
    __builtin_trap();
}

bool sameDiagnostics(
    const std::vector<talas::generative::ContextFreeDiagnostic>& left,
    const std::vector<talas::generative::ContextFreeDiagnostic>& right) noexcept
{
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index)
    {
        if (left[index].code != right[index].code
            || left[index].range != right[index].range
            || left[index].message != right[index].message)
        {
            return false;
        }
    }
    return true;
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

struct ReferenceGeneration
{
    talas::generative::ContextFreeGenerateStatus status =
        talas::generative::ContextFreeGenerateStatus::InvalidPreparedProgram;
    std::vector<std::uint16_t> output;
    std::size_t rewritePasses = 0;
    std::size_t operations = 0;
};

ReferenceGeneration referenceGenerate(
    const talas::generative::PreparedContextFreeGenerator& program)
{
    using Status = talas::generative::ContextFreeGenerateStatus;
    ReferenceGeneration result;
    std::vector<std::uint16_t> current;
    std::vector<std::uint16_t> next;
    current.reserve(program.maximumOutputSymbols);
    next.reserve(program.maximumOutputSymbols);
    for (const auto symbol : program.startSymbols)
    {
        if (++result.operations
                > talas::generative::maximumContextFreeOperations
            || current.size() >= program.maximumOutputSymbols)
        {
            result.status = current.size() >= program.maximumOutputSymbols
                ? Status::OutputLimitExceeded
                : Status::OperationLimitExceeded;
            return result;
        }
        current.push_back(symbol);
    }
    auto randomState = program.seed;
    while (true)
    {
        const auto complete = std::ranges::all_of(
            current,
            [&](std::uint16_t symbol) {
                return symbol < program.terminalFlags.size()
                    && program.terminalFlags[symbol] != 0;
            });
        if (complete)
        {
            for (const auto symbol : current)
            {
                if (++result.operations
                    > talas::generative::maximumContextFreeOperations)
                {
                    result.status = Status::OperationLimitExceeded;
                    return result;
                }
                result.output.push_back(symbol);
            }
            result.status = Status::Succeeded;
            return result;
        }
        if (result.rewritePasses >= program.maximumRewritePasses)
        {
            result.status = Status::RewriteLimitReached;
            return result;
        }
        next.clear();
        for (const auto symbol : current)
        {
            if (++result.operations
                > talas::generative::maximumContextFreeOperations)
            {
                result.status = Status::OperationLimitExceeded;
                return result;
            }
            if (program.terminalFlags[symbol] != 0)
            {
                if (next.size() >= program.maximumOutputSymbols)
                {
                    result.status = Status::OutputLimitExceeded;
                    return result;
                }
                next.push_back(symbol);
                continue;
            }
            const auto& rule = program.rules[
                static_cast<std::size_t>(program.ruleBySymbol[symbol])];
            const auto divisor =
                static_cast<std::uint64_t>(rule.alternativeCount);
            const auto threshold = (0ULL - divisor) % divisor;
            std::uint64_t word = 0;
            do
            {
                if (++result.operations
                    > talas::generative::maximumContextFreeOperations)
                {
                    result.status = Status::OperationLimitExceeded;
                    return result;
                }
                word = referenceNextRandomWord(randomState);
            }
            while (word < threshold);
            const auto& alternative = program.alternatives[
                rule.firstAlternative
                + static_cast<std::size_t>(word % divisor)];
            for (std::size_t index = 0;
                 index < alternative.symbolCount;
                 ++index)
            {
                if (next.size() >= program.maximumOutputSymbols)
                {
                    result.status = Status::OutputLimitExceeded;
                    return result;
                }
                if (++result.operations
                    > talas::generative::maximumContextFreeOperations)
                {
                    result.status = Status::OperationLimitExceeded;
                    return result;
                }
                next.push_back(program.replacementSymbols[
                    alternative.firstSymbol + index]);
            }
        }
        ++result.rewritePasses;
        current.swap(next);
    }
}
}

extern "C" int LLVMFuzzerTestOneInput(
    const std::uint8_t* data,
    std::size_t size)
{
    if (size > talas::generative::maximumContextFreeSourceBytes)
        return 0;
    std::uint64_t seed = 0;
    for (std::size_t index = 0; index < std::min(size, sizeof(seed)); ++index)
    {
        seed |= static_cast<std::uint64_t>(data[index])
            << static_cast<unsigned>(index * 8U);
    }
    const auto terminalCount = size == 0
        ? 0U
        : static_cast<unsigned>(data[0] % 9U);
    std::vector<std::string> terminals;
    terminals.reserve(terminalCount);
    for (unsigned index = 0; index < terminalCount; ++index)
        terminals.push_back("t" + std::to_string(index));
    std::string grammar;
    if (size != 0)
    {
        grammar.assign(
            reinterpret_cast<const char*>(data), size);
    }
    const talas::generative::ContextFreeGeneratorSource source{
        std::move(terminals),
        grammar,
        seed,
        static_cast<std::uint16_t>(seed
            % (talas::generative::maximumContextFreeRewritePasses + 1U)),
        static_cast<std::uint16_t>(
            1U + seed % talas::generative::maximumContextFreeOutputSymbols),
    };
    const auto firstParse = talas::generative::parseContextFreeGenerator(source);
    const auto secondParse = talas::generative::parseContextFreeGenerator(source);
    if (firstParse.ok() != secondParse.ok()
        || firstParse.truncated != secondParse.truncated
        || !sameDiagnostics(firstParse.diagnostics, secondParse.diagnostics))
    {
        invariantFailure();
    }
    if (!firstParse.ok())
        return 0;
    const auto firstPrepared = talas::generative::prepareContextFreeGenerator(source);
    const auto secondPrepared = talas::generative::prepareContextFreeGenerator(source);
    if (!firstPrepared.ok() || !secondPrepared.ok()
        || firstPrepared.program.canonicalChecksum
            != secondPrepared.program.canonicalChecksum)
    {
        invariantFailure();
    }
    std::array<std::uint16_t,
        talas::generative::maximumContextFreeOutputSymbols> firstCurrent{};
    std::array<std::uint16_t,
        talas::generative::maximumContextFreeOutputSymbols> firstNext{};
    std::array<std::uint16_t,
        talas::generative::maximumContextFreeOutputSymbols> firstOutput{};
    std::array<std::uint16_t,
        talas::generative::maximumContextFreeOutputSymbols> secondCurrent{};
    std::array<std::uint16_t,
        talas::generative::maximumContextFreeOutputSymbols> secondNext{};
    std::array<std::uint16_t,
        talas::generative::maximumContextFreeOutputSymbols> secondOutput{};
    const auto first = talas::generative::generateContextFreeSymbols(
        firstPrepared.program,
        {firstCurrent, firstNext, firstOutput});
    const auto second = talas::generative::generateContextFreeSymbols(
        secondPrepared.program,
        {secondCurrent, secondNext, secondOutput});
    if (first.status != second.status
        || first.outputCount != second.outputCount
        || first.rewritePasses != second.rewritePasses
        || first.operations != second.operations)
    {
        invariantFailure();
    }
    const auto firstSymbols = talas::generative::generatedContextFreeSymbolIndices(
        {firstCurrent, firstNext, firstOutput}, first);
    const auto secondSymbols = talas::generative::generatedContextFreeSymbolIndices(
        {secondCurrent, secondNext, secondOutput}, second);
    if (!std::ranges::equal(firstSymbols, secondSymbols))
        invariantFailure();
    const auto reference = referenceGenerate(firstPrepared.program);
    if (first.status != reference.status
        || first.outputCount != reference.output.size()
        || first.rewritePasses != reference.rewritePasses
        || first.operations != reference.operations
        || !std::ranges::equal(firstSymbols, reference.output))
    {
        invariantFailure();
    }
    for (const auto symbol : firstSymbols)
    {
        if (symbol >= firstPrepared.program.terminalFlags.size()
            || firstPrepared.program.terminalFlags[symbol] == 0)
        {
            invariantFailure();
        }
    }
    return 0;
}
