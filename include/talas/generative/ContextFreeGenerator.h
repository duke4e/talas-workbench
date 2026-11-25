#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace talas::generative
{
inline constexpr std::size_t maximumContextFreePrograms = 64;
inline constexpr std::size_t maximumContextFreeSymbols = 128;
inline constexpr std::size_t maximumContextFreeRules = 128;
inline constexpr std::size_t maximumContextFreeAlternativesPerRule = 16;
inline constexpr std::size_t maximumContextFreeSymbolsPerAlternative = 64;
inline constexpr std::size_t maximumContextFreeStartSymbols = 64;
inline constexpr std::size_t maximumContextFreeRewritePasses = 128;
inline constexpr std::size_t maximumContextFreeOutputSymbols = 4096;
inline constexpr std::size_t maximumContextFreeOperations = 65536;
inline constexpr std::size_t maximumContextFreeSymbolBytes = 128;
inline constexpr std::size_t maximumContextFreeDisplayNameBytes = 256;
inline constexpr std::size_t maximumContextFreeSourceBytes = 64U * 1024U;
inline constexpr std::size_t maximumContextFreeProjectSourceBytes = 1024U * 1024U;
inline constexpr std::size_t maximumContextFreeDiagnostics = 64;

struct ContextFreeSourceRange
{
    std::size_t begin = 0;
    std::size_t end = 0;

    bool operator==(const ContextFreeSourceRange&) const = default;
};

enum class ContextFreeDiagnosticCode
{
    SourceTooLarge,
    InvalidLineEnding,
    UnexpectedCharacter,
    InvalidIdentifier,
    ReservedIdentifier,
    ExpectedColon,
    ExpectedArrow,
    ExpectedSymbol,
    ExpectedLineEnd,
    DuplicateStart,
    MissingStart,
    DuplicateTerminal,
    DuplicateRule,
    TerminalHasRule,
    UnknownSymbol,
    TooManySymbols,
    TooManyRules,
    TooManyAlternatives,
    TooManySymbolsInSequence,
    InvalidRewriteLimit,
    InvalidOutputLimit,
    DiagnosticLimitExceeded
};

struct ContextFreeDiagnostic
{
    ContextFreeDiagnosticCode code =
        ContextFreeDiagnosticCode::UnexpectedCharacter;
    ContextFreeSourceRange range;
    std::string message;
};

struct ContextFreeGeneratorSource
{
    std::vector<std::string> terminals;
    std::string grammar;
    std::uint64_t seed = 0;
    std::uint16_t maximumRewritePasses = 0;
    std::uint16_t maximumOutputSymbols = 1;

    bool operator==(const ContextFreeGeneratorSource&) const = default;
};

struct ContextFreeAlternative
{
    std::vector<std::string> symbols;
    std::vector<ContextFreeSourceRange> symbolRanges;
    ContextFreeSourceRange range;

    bool operator==(const ContextFreeAlternative&) const = default;
};

struct ContextFreeRule
{
    std::string symbol;
    ContextFreeSourceRange symbolRange;
    std::vector<ContextFreeAlternative> alternatives;

    bool operator==(const ContextFreeRule&) const = default;
};

struct ParsedContextFreeGenerator
{
    ContextFreeGeneratorSource source;
    std::vector<std::string> startSymbols;
    std::vector<ContextFreeSourceRange> startSymbolRanges;
    std::vector<ContextFreeRule> rules;

    bool operator==(const ParsedContextFreeGenerator&) const = default;
};

struct ParseContextFreeGeneratorResult
{
    ParsedContextFreeGenerator program;
    std::vector<ContextFreeDiagnostic> diagnostics;
    bool truncated = false;

    [[nodiscard]] bool ok() const noexcept
    {
        return diagnostics.empty() && !truncated;
    }
};

[[nodiscard]] bool isValidContextFreeIdentifier(
    std::string_view value) noexcept;

[[nodiscard]] ParseContextFreeGeneratorResult parseContextFreeGenerator(
    const ContextFreeGeneratorSource& source);

struct PreparedContextFreeAlternative
{
    std::uint32_t firstSymbol = 0;
    std::uint16_t symbolCount = 0;
};

struct PreparedContextFreeRule
{
    std::uint16_t symbolIndex = 0;
    std::uint32_t firstAlternative = 0;
    std::uint16_t alternativeCount = 0;
};

struct PreparedContextFreeGenerator
{
    std::vector<std::string> symbols;
    std::vector<std::uint8_t> terminalFlags;
    std::vector<std::int16_t> ruleBySymbol;
    std::vector<std::uint16_t> startSymbols;
    std::vector<PreparedContextFreeRule> rules;
    std::vector<PreparedContextFreeAlternative> alternatives;
    std::vector<std::uint16_t> replacementSymbols;
    std::uint64_t seed = 0;
    std::uint16_t maximumRewritePasses = 0;
    std::uint16_t maximumOutputSymbols = 0;
    std::uint64_t canonicalChecksum = 0;
};

struct PrepareContextFreeGeneratorResult
{
    PreparedContextFreeGenerator program;
    std::vector<ContextFreeDiagnostic> diagnostics;
    bool truncated = false;

    [[nodiscard]] bool ok() const noexcept
    {
        return diagnostics.empty() && !truncated;
    }
};

[[nodiscard]] PrepareContextFreeGeneratorResult prepareContextFreeGenerator(
    const ContextFreeGeneratorSource& source);

struct ContextFreeGeneratorScratch
{
    std::span<std::uint16_t> current;
    std::span<std::uint16_t> next;
    std::span<std::uint16_t> output;
};

enum class ContextFreeGenerateStatus
{
    Succeeded,
    InvalidPreparedProgram,
    InsufficientScratch,
    AliasedScratch,
    RewriteLimitReached,
    OutputLimitExceeded,
    OperationLimitExceeded,
    Cancelled
};

struct ContextFreeGenerateResult
{
    ContextFreeGenerateStatus status =
        ContextFreeGenerateStatus::InvalidPreparedProgram;
    std::size_t outputCount = 0;
    std::size_t rewritePasses = 0;
    std::size_t operations = 0;

    [[nodiscard]] bool ok() const noexcept
    {
        return status == ContextFreeGenerateStatus::Succeeded;
    }
};

[[nodiscard]] ContextFreeGenerateResult generateContextFreeSymbols(
    const PreparedContextFreeGenerator& program,
    ContextFreeGeneratorScratch scratch,
    std::stop_token stopToken = {}) noexcept;

[[nodiscard]] std::span<const std::uint16_t> generatedContextFreeSymbolIndices(
    const ContextFreeGeneratorScratch& scratch,
    const ContextFreeGenerateResult& result) noexcept;

[[nodiscard]] std::uint64_t contextFreeNextRandomWord(
    std::uint64_t& state) noexcept;
}
