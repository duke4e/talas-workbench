#include "talas/generative/ContextFreeGenerator.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <vector>

namespace
{
std::atomic<bool> countAllocations = false;
std::atomic<std::size_t> allocationCount = 0;
std::atomic<std::size_t> allocationBytes = 0;
}

void* operator new(std::size_t size)
{
    if (countAllocations.load(std::memory_order_relaxed))
    {
        allocationCount.fetch_add(1, std::memory_order_relaxed);
        allocationBytes.fetch_add(size, std::memory_order_relaxed);
    }
    if (auto* memory = std::malloc(size))
        return memory;
    throw std::bad_alloc{};
}

void* operator new[](std::size_t size)
{
    if (countAllocations.load(std::memory_order_relaxed))
    {
        allocationCount.fetch_add(1, std::memory_order_relaxed);
        allocationBytes.fetch_add(size, std::memory_order_relaxed);
    }
    if (auto* memory = std::malloc(size))
        return memory;
    throw std::bad_alloc{};
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace
{
class AllocationCounter
{
public:
    AllocationCounter()
    {
        allocationCount.store(0, std::memory_order_relaxed);
        allocationBytes.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_relaxed);
    }

    ~AllocationCounter()
    {
        countAllocations.store(false, std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t count() const noexcept
    {
        return allocationCount.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t bytes() const noexcept
    {
        return allocationBytes.load(std::memory_order_relaxed);
    }
};

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

talas::generative::ContextFreeGeneratorSource source(
    std::string grammar,
    std::vector<std::string> terminals = {"note", "rest"},
    std::uint64_t seed = 0,
    std::uint16_t passes = 16,
    std::uint16_t output = 64)
{
    return {
        std::move(terminals),
        std::move(grammar),
        seed,
        passes,
        output,
    };
}

struct Generated
{
    talas::generative::ContextFreeGenerateResult result;
    std::vector<std::string> symbols;
};

Generated generate(
    const talas::generative::PreparedContextFreeGenerator& program,
    std::stop_token stopToken = {})
{
    std::array<std::uint16_t,
        talas::generative::maximumContextFreeOutputSymbols> current{};
    std::array<std::uint16_t,
        talas::generative::maximumContextFreeOutputSymbols> next{};
    std::array<std::uint16_t,
        talas::generative::maximumContextFreeOutputSymbols> output{};
    talas::generative::ContextFreeGeneratorScratch scratch{
        current, next, output};
    Generated generated;
    generated.result = talas::generative::generateContextFreeSymbols(
        program, scratch, stopToken);
    for (const auto index :
         talas::generative::generatedContextFreeSymbolIndices(
             scratch, generated.result))
    {
        require(index < program.symbols.size(),
            "generated symbol index should be prepared");
        generated.symbols.push_back(program.symbols[index]);
    }
    return generated;
}

void testIdentifierAndSplitMixProtocol()
{
    using talas::generative::isValidContextFreeIdentifier;
    require(isValidContextFreeIdentifier("note_1-a"),
        "closed ASCII identifier should be accepted");
    require(!isValidContextFreeIdentifier("")
            && !isValidContextFreeIdentifier("1note")
            && !isValidContextFreeIdentifier("note.a")
            && !isValidContextFreeIdentifier("ž"),
        "empty, numeric-first, punctuated, and non-ASCII identities should reject");

    std::uint64_t state = 0;
    require(talas::generative::contextFreeNextRandomWord(state)
                == 0xe220a8397b1dcdafULL
            && talas::generative::contextFreeNextRandomWord(state)
                == 0x6e789e6aa1b965f4ULL,
        "SplitMix64 protocol must retain its exact golden words");
}

void testParserRetainsExactRangesAndLineEndingParity()
{
    const auto lf = source(
        "start: phrase\n"
        "phrase -> note phrase | rest phrase | note\n",
        {"note", "rest"},
        11);
    auto crlf = lf;
    crlf.grammar =
        "start: phrase\r\n"
        "phrase -> note phrase | rest phrase | note\r\n";
    const auto parsed = talas::generative::parseContextFreeGenerator(lf);
    require(parsed.ok()
            && parsed.program.startSymbols
                == std::vector<std::string>{"phrase"}
            && parsed.program.rules.size() == 1
            && parsed.program.rules.front().alternatives.size() == 3,
        "valid grammar should parse into one start sequence and ordered alternatives");
    const auto preparedLf = talas::generative::prepareContextFreeGenerator(lf);
    const auto preparedCrlf = talas::generative::prepareContextFreeGenerator(crlf);
    require(preparedLf.ok() && preparedCrlf.ok()
            && preparedLf.program.canonicalChecksum
                == preparedCrlf.program.canonicalChecksum,
        "LF and CRLF spelling should prepare the same canonical program");

    const auto unknown = talas::generative::parseContextFreeGenerator(
        source("start: missing\n", {"note"}));
    require(!unknown.ok() && unknown.diagnostics.size() == 1
            && unknown.diagnostics.front().code
                == talas::generative::ContextFreeDiagnosticCode::UnknownSymbol
            && unknown.diagnostics.front().range
                == talas::generative::ContextFreeSourceRange{7, 14},
        "unknown start symbol should retain its exact source range");

    const auto bareCr = talas::generative::parseContextFreeGenerator(
        source("start: epsilon\r", {}));
    require(!bareCr.ok()
            && std::ranges::any_of(
                bareCr.diagnostics,
                [](const auto& diagnostic) {
                    return diagnostic.code
                        == talas::generative::ContextFreeDiagnosticCode::InvalidLineEnding;
                }),
        "bare CR should reject instead of changing canonical line semantics");
}

void testMalformedCallerSourceRejectsBeforeDeepCopy()
{
    auto oversized = source("start: note\n", {
        std::string(1024U * 1024U, 'x')});
    talas::generative::ParseContextFreeGeneratorResult parsed;
    std::size_t bytes = 0;
    {
        AllocationCounter counter;
        parsed = talas::generative::parseContextFreeGenerator(oversized);
        bytes = counter.bytes();
    }
    require(
        !parsed.ok() && bytes < 16U * 1024U,
        "oversized caller-owned terminals should reject before deep copy");
}

void testDeterministicBoundedGeneration()
{
    const auto prepared = talas::generative::prepareContextFreeGenerator(source(
        "start: phrase\n"
        "phrase -> note phrase | rest phrase | note\n",
        {"note", "rest"},
        0,
        16,
        64));
    require(prepared.ok(), "deterministic fixture should prepare");
    const auto first = generate(prepared.program);
    const auto second = generate(prepared.program);
    require(first.result.ok() && second.result.ok()
            && first.symbols == second.symbols
            && first.symbols == std::vector<std::string>{
                "rest", "note", "rest", "rest", "rest", "note", "note"},
        "same prepared source and seed should produce its exact golden terminal sequence");

    std::array<std::uint16_t,
        talas::generative::maximumContextFreeOutputSymbols> current{};
    std::array<std::uint16_t,
        talas::generative::maximumContextFreeOutputSymbols> next{};
    std::array<std::uint16_t,
        talas::generative::maximumContextFreeOutputSymbols> output{};
    const talas::generative::ContextFreeGeneratorScratch scratch{
        current, next, output};
    talas::generative::ContextFreeGenerateResult allocationResult;
    std::size_t allocations = 0;
    {
        AllocationCounter counter;
        allocationResult = talas::generative::generateContextFreeSymbols(
            prepared.program, scratch);
        allocations = counter.count();
    }
    require(allocationResult.ok() && allocations == 0,
        "fixed-scratch generator execution must not allocate");

    const auto terminalOnly = talas::generative::prepareContextFreeGenerator(
        source("start: note rest\n", {"note", "rest"}, 9, 0, 2));
    const auto terminalOutput = generate(terminalOnly.program);
    require(terminalOnly.ok() && terminalOutput.result.ok()
            && terminalOutput.result.rewritePasses == 0
            && terminalOutput.symbols
                == std::vector<std::string>{"note", "rest"},
        "terminal-only start should not consume a rewrite pass");

    const auto empty = talas::generative::prepareContextFreeGenerator(
        source("start: epsilon\n", {}, 4, 0, 1));
    const auto emptyOutput = generate(empty.program);
    require(empty.ok() && emptyOutput.result.ok()
            && emptyOutput.symbols.empty(),
        "explicit epsilon start should generate deterministic empty output");
}

void testGenerationFailuresPublishNoPrefix()
{
    const auto rewriteLimited = talas::generative::prepareContextFreeGenerator(
        source("start: A\nA -> A\n", {}, 0, 1, 8));
    require(rewriteLimited.ok(), "rewrite-limit fixture should prepare");
    const auto limited = generate(rewriteLimited.program);
    require(limited.result.status
                == talas::generative::ContextFreeGenerateStatus::RewriteLimitReached
            && limited.result.outputCount == 0
            && limited.symbols.empty(),
        "unfinished nonterminal should fail without a partial prefix");

    const auto outputLimited = talas::generative::prepareContextFreeGenerator(
        source("start: A\nA -> note note\n", {"note"}, 0, 1, 1));
    require(outputLimited.ok(), "output-limit fixture should prepare");
    std::array<std::uint16_t, 1> current{};
    std::array<std::uint16_t, 1> next{};
    std::array<std::uint16_t, 1> output{0xbeefU};
    const talas::generative::ContextFreeGeneratorScratch scratch{
        current, next, output};
    const auto failed = talas::generative::generateContextFreeSymbols(
        outputLimited.program, scratch);
    require(failed.status
                == talas::generative::ContextFreeGenerateStatus::OutputLimitExceeded
            && failed.outputCount == 0
            && output.front() == 0xbeefU,
        "capacity exhaustion should retain caller output bytes");

    const auto aliased = talas::generative::generateContextFreeSymbols(
        outputLimited.program, {current, current, output});
    require(aliased.status
                == talas::generative::ContextFreeGenerateStatus::AliasedScratch
            && aliased.outputCount == 0,
        "overlapping scratch buffers should reject before mutation");

    std::stop_source stop;
    stop.request_stop();
    const auto cancelled = generate(rewriteLimited.program, stop.get_token());
    require(cancelled.result.status
                == talas::generative::ContextFreeGenerateStatus::Cancelled
            && cancelled.result.outputCount == 0,
        "pre-cancelled generation should publish no output");

    auto malformed = rewriteLimited.program;
    malformed.startSymbols.front() = 65000;
    const auto invalid = generate(malformed);
    require(invalid.result.status
                == talas::generative::ContextFreeGenerateStatus::InvalidPreparedProgram
            && invalid.result.outputCount == 0,
        "invalid prepared indices should fail closed");

    malformed = rewriteLimited.program;
    malformed.alternatives.front().symbolCount = 65;
    require(
        generate(malformed).result.status
            == talas::generative::ContextFreeGenerateStatus::InvalidPreparedProgram,
        "oversized prepared alternatives should fail as invalid authority");
    malformed = rewriteLimited.program;
    malformed.rules.front().alternativeCount = 17;
    malformed.alternatives.resize(17);
    require(
        generate(malformed).result.status
            == talas::generative::ContextFreeGenerateStatus::InvalidPreparedProgram,
        "oversized prepared rule choices should fail as invalid authority");
}

void testClosedGrammarValidation()
{
    const std::array invalid{
        source("phrase -> note\n", {"note"}),
        source("start phrase\n", {"note"}),
        source("start: A\nA ->\n", {}),
        source("start: A\nA -> note |\n", {"note"}),
        source("start: A\nA -> note\nA -> note\n", {"note"}),
        source("start: note\nnote -> epsilon\n", {"note"}),
    };
    for (const auto& candidate : invalid)
    {
        require(!talas::generative::parseContextFreeGenerator(candidate).ok(),
            "malformed or ambiguous grammar should reject");
    }
}
}

int main()
{
    try
    {
        testIdentifierAndSplitMixProtocol();
        testParserRetainsExactRangesAndLineEndingParity();
        testMalformedCallerSourceRejectsBeforeDeepCopy();
        testDeterministicBoundedGeneration();
        testGenerationFailuresPublishNoPrefix();
        testClosedGrammarValidation();
    }
    catch (const std::exception& exception)
    {
        std::cerr << "ContextFreeGeneratorTests failed: "
                  << exception.what() << '\n';
        return 1;
    }
    return 0;
}
