#include "talas/generative/ContextFreeGenerator.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <ranges>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace talas::generative
{
namespace
{
enum class TokenKind
{
    Identifier,
    Colon,
    Arrow,
    Pipe,
    Newline,
    End,
    Invalid
};

struct Token
{
    TokenKind kind = TokenKind::Invalid;
    ContextFreeSourceRange range;
    std::string_view text;
};

class Lexer
{
public:
    explicit Lexer(std::string_view source) noexcept : source_(source) {}

    [[nodiscard]] Token next() noexcept
    {
        while (offset_ < source_.size()
            && (source_[offset_] == ' ' || source_[offset_] == '\t'))
        {
            ++offset_;
        }
        if (offset_ == source_.size())
            return {TokenKind::End, {offset_, offset_}, {}};

        const auto begin = offset_;
        const auto character = source_[offset_];
        if (character == '\n')
        {
            ++offset_;
            return {TokenKind::Newline, {begin, offset_}, source_.substr(begin, 1)};
        }
        if (character == '\r')
        {
            ++offset_;
            if (offset_ < source_.size() && source_[offset_] == '\n')
            {
                ++offset_;
                return {
                    TokenKind::Newline,
                    {begin, offset_},
                    source_.substr(begin, 2)};
            }
            return {TokenKind::Invalid, {begin, offset_}, source_.substr(begin, 1)};
        }
        if (character == ':')
        {
            ++offset_;
            return {TokenKind::Colon, {begin, offset_}, source_.substr(begin, 1)};
        }
        if (character == '|')
        {
            ++offset_;
            return {TokenKind::Pipe, {begin, offset_}, source_.substr(begin, 1)};
        }
        if (character == '-' && offset_ + 1 < source_.size()
            && source_[offset_ + 1] == '>')
        {
            offset_ += 2;
            return {TokenKind::Arrow, {begin, offset_}, source_.substr(begin, 2)};
        }
        const auto unsignedCharacter = static_cast<unsigned char>(character);
        if ((unsignedCharacter >= 'A' && unsignedCharacter <= 'Z')
            || (unsignedCharacter >= 'a' && unsignedCharacter <= 'z')
            || character == '_')
        {
            ++offset_;
            while (offset_ < source_.size())
            {
                const auto nextCharacter =
                    static_cast<unsigned char>(source_[offset_]);
                if (!((nextCharacter >= 'A' && nextCharacter <= 'Z')
                        || (nextCharacter >= 'a' && nextCharacter <= 'z')
                        || (nextCharacter >= '0' && nextCharacter <= '9')
                        || source_[offset_] == '_'
                        || source_[offset_] == '-'))
                {
                    break;
                }
                ++offset_;
            }
            return {
                TokenKind::Identifier,
                {begin, offset_},
                source_.substr(begin, offset_ - begin)};
        }
        ++offset_;
        return {TokenKind::Invalid, {begin, offset_}, source_.substr(begin, 1)};
    }

private:
    std::string_view source_;
    std::size_t offset_ = 0;
};

void appendDiagnostic(
    ParseContextFreeGeneratorResult& result,
    ContextFreeDiagnosticCode code,
    ContextFreeSourceRange range,
    std::string message)
{
    if (result.truncated)
        return;
    if (result.diagnostics.size() >= maximumContextFreeDiagnostics)
    {
        result.truncated = true;
        return;
    }
    result.diagnostics.push_back(
        {code, range, std::move(message)});
}

[[nodiscard]] bool reservedIdentifier(std::string_view value) noexcept
{
    return value == "start" || value == "epsilon";
}

void skipLine(Lexer& lexer, Token& token) noexcept
{
    while (token.kind != TokenKind::Newline
        && token.kind != TokenKind::End)
    {
        token = lexer.next();
    }
    if (token.kind == TokenKind::Newline)
        token = lexer.next();
}

[[nodiscard]] bool appendSequenceSymbol(
    ParseContextFreeGeneratorResult& result,
    std::vector<std::string>& symbols,
    std::vector<ContextFreeSourceRange>& ranges,
    const Token& token,
    std::size_t maximumCount)
{
    if (symbols.size() >= maximumCount)
    {
        appendDiagnostic(
            result,
            ContextFreeDiagnosticCode::TooManySymbolsInSequence,
            token.range,
            "Context-free sequence exceeds its symbol limit");
        return false;
    }
    if (!isValidContextFreeIdentifier(token.text)
        || reservedIdentifier(token.text))
    {
        appendDiagnostic(
            result,
            reservedIdentifier(token.text)
                ? ContextFreeDiagnosticCode::ReservedIdentifier
                : ContextFreeDiagnosticCode::InvalidIdentifier,
            token.range,
            "Context-free sequence contains an invalid or reserved symbol");
        return false;
    }
    symbols.emplace_back(token.text);
    ranges.push_back(token.range);
    return true;
}

[[nodiscard]] std::uint64_t fnvAppend(
    std::uint64_t hash,
    std::span<const std::byte> bytes) noexcept
{
    constexpr std::uint64_t prime = 1099511628211ULL;
    for (const auto byte : bytes)
    {
        hash ^= std::to_integer<std::uint8_t>(byte);
        hash *= prime;
    }
    return hash;
}

[[nodiscard]] std::uint64_t hashU64(
    std::uint64_t hash,
    std::uint64_t value) noexcept
{
    std::array<std::byte, 8> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index)
    {
        bytes[index] = static_cast<std::byte>(
            (value >> static_cast<unsigned>(index * 8U)) & 0xffU);
    }
    return fnvAppend(hash, bytes);
}

[[nodiscard]] std::uint64_t hashString(
    std::uint64_t hash,
    std::string_view value) noexcept
{
    hash = hashU64(hash, value.size());
    return fnvAppend(
        hash,
        std::as_bytes(std::span(value.data(), value.size())));
}

[[nodiscard]] bool consumeOperation(
    std::size_t& operations,
    std::stop_token stopToken) noexcept
{
    if (operations >= maximumContextFreeOperations)
        return false;
    ++operations;
    return (operations & 0xffU) != 0U || !stopToken.stop_requested();
}

[[nodiscard]] bool spansOverlap(
    std::span<std::uint16_t> left,
    std::span<std::uint16_t> right) noexcept
{
    if (left.empty() || right.empty())
        return false;
    const auto leftBegin = reinterpret_cast<std::uintptr_t>(left.data());
    const auto rightBegin = reinterpret_cast<std::uintptr_t>(right.data());
    const auto leftEnd = leftBegin + left.size_bytes();
    const auto rightEnd = rightBegin + right.size_bytes();
    return leftBegin < rightEnd && rightBegin < leftEnd;
}

[[nodiscard]] std::optional<std::size_t> boundedRandom(
    std::uint64_t& state,
    std::size_t bound,
    std::size_t& operations,
    std::stop_token stopToken) noexcept
{
    if (bound == 0 || bound > maximumContextFreeAlternativesPerRule)
        return std::nullopt;
    const auto divisor = static_cast<std::uint64_t>(bound);
    const auto threshold = (0ULL - divisor) % divisor;
    while (true)
    {
        if (!consumeOperation(operations, stopToken))
            return std::nullopt;
        const auto word = contextFreeNextRandomWord(state);
        if (word >= threshold)
            return static_cast<std::size_t>(word % divisor);
    }
}
}

bool isValidContextFreeIdentifier(std::string_view value) noexcept
{
    if (value.empty() || value.size() > maximumContextFreeSymbolBytes)
        return false;
    const auto first = static_cast<unsigned char>(value.front());
    if (!((first >= 'A' && first <= 'Z')
            || (first >= 'a' && first <= 'z')
            || value.front() == '_'))
    {
        return false;
    }
    return std::ranges::all_of(
        value.substr(1),
        [](unsigned char character) {
            return (character >= 'A' && character <= 'Z')
                || (character >= 'a' && character <= 'z')
                || (character >= '0' && character <= '9')
                || character == '_'
                || character == '-';
        });
}

ParseContextFreeGeneratorResult parseContextFreeGenerator(
    const ContextFreeGeneratorSource& source)
{
    ParseContextFreeGeneratorResult result;
    if (source.grammar.size() > maximumContextFreeSourceBytes)
    {
        appendDiagnostic(
            result,
            ContextFreeDiagnosticCode::SourceTooLarge,
            {0, source.grammar.size()},
            "Context-free grammar exceeds its byte limit");
        return result;
    }
    for (std::size_t index = 0; index < source.grammar.size(); ++index)
    {
        if (source.grammar[index] == '\r'
            && (index + 1 == source.grammar.size()
                || source.grammar[index + 1] != '\n'))
        {
            appendDiagnostic(
                result,
                ContextFreeDiagnosticCode::InvalidLineEnding,
                {index, index + 1},
                "Context-free grammar accepts LF or CRLF line endings only");
        }
    }
    if (source.maximumRewritePasses > maximumContextFreeRewritePasses)
    {
        appendDiagnostic(
            result,
            ContextFreeDiagnosticCode::InvalidRewriteLimit,
            {},
            "Context-free rewrite limit exceeds the repository maximum");
    }
    if (source.maximumOutputSymbols == 0
        || source.maximumOutputSymbols > maximumContextFreeOutputSymbols)
    {
        appendDiagnostic(
            result,
            ContextFreeDiagnosticCode::InvalidOutputLimit,
            {},
            "Context-free output limit must be within 1..4096");
    }
    if (source.terminals.size() > maximumContextFreeSymbols)
    {
        appendDiagnostic(
            result,
            ContextFreeDiagnosticCode::TooManySymbols,
            {},
            "Context-free terminal count exceeds its limit");
        return result;
    }
    for (const auto& terminal : source.terminals)
    {
        if (terminal.size() > maximumContextFreeSymbolBytes)
        {
            appendDiagnostic(
                result,
                ContextFreeDiagnosticCode::InvalidIdentifier,
                {},
                "Context-free terminal exceeds its byte limit");
            return result;
        }
    }
    // Copy only after every caller-owned variable-size field is proven bounded.
    result.program.source = source;
    std::unordered_set<std::string_view> terminalSet;
    terminalSet.reserve(std::min(
        source.terminals.size(), maximumContextFreeSymbols));
    for (const auto& terminal : source.terminals)
    {
        if (!isValidContextFreeIdentifier(terminal)
            || reservedIdentifier(terminal))
        {
            appendDiagnostic(
                result,
                reservedIdentifier(terminal)
                    ? ContextFreeDiagnosticCode::ReservedIdentifier
                    : ContextFreeDiagnosticCode::InvalidIdentifier,
                {},
                "Context-free terminal has an invalid or reserved identity");
            continue;
        }
        if (!terminalSet.insert(terminal).second)
        {
            appendDiagnostic(
                result,
                ContextFreeDiagnosticCode::DuplicateTerminal,
                {},
                "Context-free terminal identities must be unique");
        }
    }

    Lexer lexer(source.grammar);
    auto token = lexer.next();
    bool hasStart = false;
    while (token.kind != TokenKind::End)
    {
        if (token.kind == TokenKind::Newline)
        {
            token = lexer.next();
            continue;
        }
        if (token.kind == TokenKind::Invalid)
        {
            appendDiagnostic(
                result,
                token.text == "\r"
                    ? ContextFreeDiagnosticCode::InvalidLineEnding
                    : ContextFreeDiagnosticCode::UnexpectedCharacter,
                token.range,
                "Context-free grammar contains an unexpected character");
            skipLine(lexer, token);
            continue;
        }
        if (token.kind != TokenKind::Identifier)
        {
            appendDiagnostic(
                result,
                ContextFreeDiagnosticCode::ExpectedSymbol,
                token.range,
                "Context-free grammar line must begin with a symbol");
            skipLine(lexer, token);
            continue;
        }

        const auto lineSymbol = token;
        token = lexer.next();
        if (lineSymbol.text == "start")
        {
            if (hasStart)
            {
                appendDiagnostic(
                    result,
                    ContextFreeDiagnosticCode::DuplicateStart,
                    lineSymbol.range,
                    "Context-free grammar accepts exactly one start line");
            }
            hasStart = true;
            if (token.kind != TokenKind::Colon)
            {
                appendDiagnostic(
                    result,
                    ContextFreeDiagnosticCode::ExpectedColon,
                    token.range,
                    "Context-free start line requires ':'");
                skipLine(lexer, token);
                continue;
            }
            token = lexer.next();
            bool epsilon = false;
            while (token.kind != TokenKind::Newline
                && token.kind != TokenKind::End)
            {
                if (token.kind != TokenKind::Identifier)
                {
                    appendDiagnostic(
                        result,
                        ContextFreeDiagnosticCode::ExpectedSymbol,
                        token.range,
                        "Context-free start line expects symbols");
                    skipLine(lexer, token);
                    break;
                }
                if (token.text == "epsilon")
                {
                    epsilon = true;
                    token = lexer.next();
                    if (!result.program.startSymbols.empty()
                        || (token.kind != TokenKind::Newline
                            && token.kind != TokenKind::End))
                    {
                        appendDiagnostic(
                            result,
                            ContextFreeDiagnosticCode::ReservedIdentifier,
                            token.range,
                            "epsilon must be the only start-sequence token");
                        skipLine(lexer, token);
                    }
                    break;
                }
                if (!appendSequenceSymbol(
                        result,
                        result.program.startSymbols,
                        result.program.startSymbolRanges,
                        token,
                        maximumContextFreeStartSymbols))
                {
                    skipLine(lexer, token);
                    break;
                }
                token = lexer.next();
            }
            if (epsilon)
            {
                result.program.startSymbols.clear();
                result.program.startSymbolRanges.clear();
            }
            if (token.kind == TokenKind::Newline)
                token = lexer.next();
            continue;
        }

        if (!isValidContextFreeIdentifier(lineSymbol.text)
            || reservedIdentifier(lineSymbol.text))
        {
            appendDiagnostic(
                result,
                reservedIdentifier(lineSymbol.text)
                    ? ContextFreeDiagnosticCode::ReservedIdentifier
                    : ContextFreeDiagnosticCode::InvalidIdentifier,
                lineSymbol.range,
                "Context-free rule has an invalid or reserved identity");
        }
        if (token.kind != TokenKind::Arrow)
        {
            appendDiagnostic(
                result,
                ContextFreeDiagnosticCode::ExpectedArrow,
                token.range,
                "Context-free rule requires '->'");
            skipLine(lexer, token);
            continue;
        }
        ContextFreeRule rule;
        rule.symbol = std::string(lineSymbol.text);
        rule.symbolRange = lineSymbol.range;
        token = lexer.next();
        while (true)
        {
            if (rule.alternatives.size()
                >= maximumContextFreeAlternativesPerRule)
            {
                appendDiagnostic(
                    result,
                    ContextFreeDiagnosticCode::TooManyAlternatives,
                    token.range,
                    "Context-free rule exceeds its alternative limit");
                skipLine(lexer, token);
                break;
            }
            ContextFreeAlternative alternative;
            alternative.range.begin = token.range.begin;
            bool explicitEpsilon = false;
            while (token.kind == TokenKind::Identifier)
            {
                if (token.text == "epsilon")
                {
                    explicitEpsilon = true;
                    alternative.range.end = token.range.end;
                    token = lexer.next();
                    break;
                }
                if (!appendSequenceSymbol(
                        result,
                        alternative.symbols,
                        alternative.symbolRanges,
                        token,
                        maximumContextFreeSymbolsPerAlternative))
                {
                    skipLine(lexer, token);
                    break;
                }
                alternative.range.end = token.range.end;
                token = lexer.next();
            }
            if (explicitEpsilon
                && (token.kind != TokenKind::Pipe
                    && token.kind != TokenKind::Newline
                    && token.kind != TokenKind::End))
            {
                appendDiagnostic(
                    result,
                    ContextFreeDiagnosticCode::ReservedIdentifier,
                    token.range,
                    "epsilon must be the only symbol in an alternative");
                skipLine(lexer, token);
            }
            if (!explicitEpsilon && alternative.symbols.empty())
            {
                appendDiagnostic(
                    result,
                    ContextFreeDiagnosticCode::ExpectedSymbol,
                    token.range,
                    "Context-free alternatives require a symbol or epsilon");
            }
            rule.alternatives.push_back(std::move(alternative));
            if (token.kind == TokenKind::Pipe)
            {
                token = lexer.next();
                continue;
            }
            if (token.kind != TokenKind::Newline
                && token.kind != TokenKind::End)
            {
                appendDiagnostic(
                    result,
                    ContextFreeDiagnosticCode::ExpectedLineEnd,
                    token.range,
                    "Context-free rule contains trailing input");
                skipLine(lexer, token);
            }
            break;
        }
        if (result.program.rules.size() >= maximumContextFreeRules)
        {
            appendDiagnostic(
                result,
                ContextFreeDiagnosticCode::TooManyRules,
                lineSymbol.range,
                "Context-free grammar exceeds its rule limit");
        }
        else
        {
            result.program.rules.push_back(std::move(rule));
        }
        if (token.kind == TokenKind::Newline)
            token = lexer.next();
    }
    if (!hasStart)
    {
        appendDiagnostic(
            result,
            ContextFreeDiagnosticCode::MissingStart,
            {source.grammar.size(), source.grammar.size()},
            "Context-free grammar requires one start line");
    }

    std::unordered_map<std::string_view, const ContextFreeRule*> rules;
    rules.reserve(result.program.rules.size());
    for (const auto& rule : result.program.rules)
    {
        if (!rules.emplace(rule.symbol, std::addressof(rule)).second)
        {
            appendDiagnostic(
                result,
                ContextFreeDiagnosticCode::DuplicateRule,
                rule.symbolRange,
                "Context-free rule identities must be unique");
        }
        if (terminalSet.contains(rule.symbol))
        {
            appendDiagnostic(
                result,
                ContextFreeDiagnosticCode::TerminalHasRule,
                rule.symbolRange,
                "A context-free terminal cannot also own a rule");
        }
    }
    if (terminalSet.size() + rules.size() > maximumContextFreeSymbols)
    {
        appendDiagnostic(
            result,
            ContextFreeDiagnosticCode::TooManySymbols,
            {},
            "Context-free grammar exceeds its total symbol limit");
    }
    const auto validateReference = [&](
        std::string_view symbol,
        ContextFreeSourceRange range) {
        if (!terminalSet.contains(symbol) && !rules.contains(symbol))
        {
            appendDiagnostic(
                result,
                ContextFreeDiagnosticCode::UnknownSymbol,
                range,
                "Context-free sequence references an undeclared symbol");
        }
    };
    for (std::size_t index = 0;
         index < result.program.startSymbols.size();
         ++index)
    {
        validateReference(
            result.program.startSymbols[index],
            result.program.startSymbolRanges[index]);
    }
    for (const auto& rule : result.program.rules)
    {
        for (const auto& alternative : rule.alternatives)
        {
            for (std::size_t index = 0;
                 index < alternative.symbols.size();
                 ++index)
            {
                validateReference(
                    alternative.symbols[index],
                    alternative.symbolRanges[index]);
            }
        }
    }
    return result;
}

PrepareContextFreeGeneratorResult prepareContextFreeGenerator(
    const ContextFreeGeneratorSource& source)
{
    PrepareContextFreeGeneratorResult result;
    auto parsed = parseContextFreeGenerator(source);
    result.diagnostics = std::move(parsed.diagnostics);
    result.truncated = parsed.truncated;
    if (!result.ok())
        return result;

    auto& prepared = result.program;
    prepared.seed = source.seed;
    prepared.maximumRewritePasses = source.maximumRewritePasses;
    prepared.maximumOutputSymbols = source.maximumOutputSymbols;
    prepared.symbols = source.terminals;
    for (const auto& rule : parsed.program.rules)
        prepared.symbols.push_back(rule.symbol);
    prepared.terminalFlags.assign(prepared.symbols.size(), 0);
    std::fill_n(
        prepared.terminalFlags.begin(), source.terminals.size(), 1);
    prepared.ruleBySymbol.assign(prepared.symbols.size(), -1);

    std::unordered_map<std::string_view, std::uint16_t> symbolIndices;
    symbolIndices.reserve(prepared.symbols.size());
    for (std::size_t index = 0; index < prepared.symbols.size(); ++index)
    {
        symbolIndices.emplace(
            prepared.symbols[index], static_cast<std::uint16_t>(index));
    }
    for (const auto& symbol : parsed.program.startSymbols)
        prepared.startSymbols.push_back(symbolIndices.at(symbol));
    for (const auto& rule : parsed.program.rules)
    {
        PreparedContextFreeRule preparedRule;
        preparedRule.symbolIndex = symbolIndices.at(rule.symbol);
        preparedRule.firstAlternative =
            static_cast<std::uint32_t>(prepared.alternatives.size());
        preparedRule.alternativeCount =
            static_cast<std::uint16_t>(rule.alternatives.size());
        prepared.ruleBySymbol[preparedRule.symbolIndex] =
            static_cast<std::int16_t>(prepared.rules.size());
        for (const auto& alternative : rule.alternatives)
        {
            PreparedContextFreeAlternative preparedAlternative;
            preparedAlternative.firstSymbol =
                static_cast<std::uint32_t>(prepared.replacementSymbols.size());
            preparedAlternative.symbolCount =
                static_cast<std::uint16_t>(alternative.symbols.size());
            for (const auto& symbol : alternative.symbols)
                prepared.replacementSymbols.push_back(symbolIndices.at(symbol));
            prepared.alternatives.push_back(preparedAlternative);
        }
        prepared.rules.push_back(preparedRule);
    }

    constexpr std::uint64_t offsetBasis = 14695981039346656037ULL;
    auto hash = hashU64(offsetBasis, source.seed);
    hash = hashU64(hash, source.maximumRewritePasses);
    hash = hashU64(hash, source.maximumOutputSymbols);
    hash = hashU64(hash, source.terminals.size());
    for (const auto& terminal : source.terminals)
        hash = hashString(hash, terminal);
    hash = hashU64(hash, parsed.program.startSymbols.size());
    for (const auto& symbol : parsed.program.startSymbols)
        hash = hashString(hash, symbol);
    hash = hashU64(hash, parsed.program.rules.size());
    for (const auto& rule : parsed.program.rules)
    {
        hash = hashString(hash, rule.symbol);
        hash = hashU64(hash, rule.alternatives.size());
        for (const auto& alternative : rule.alternatives)
        {
            hash = hashU64(hash, alternative.symbols.size());
            for (const auto& symbol : alternative.symbols)
                hash = hashString(hash, symbol);
        }
    }
    prepared.canonicalChecksum = hash;
    return result;
}

ContextFreeGenerateResult generateContextFreeSymbols(
    const PreparedContextFreeGenerator& program,
    ContextFreeGeneratorScratch scratch,
    std::stop_token stopToken) noexcept
{
    ContextFreeGenerateResult result;
    if (program.symbols.size() > maximumContextFreeSymbols
        || program.terminalFlags.size() != program.symbols.size()
        || program.ruleBySymbol.size() != program.symbols.size()
        || program.startSymbols.size() > maximumContextFreeStartSymbols
        || program.rules.size() > maximumContextFreeRules
        || program.alternatives.size()
            > maximumContextFreeRules
                * maximumContextFreeAlternativesPerRule
        || program.replacementSymbols.size()
            > maximumContextFreeRules
                * maximumContextFreeAlternativesPerRule
                * maximumContextFreeSymbolsPerAlternative
        || program.maximumRewritePasses > maximumContextFreeRewritePasses
        || program.maximumOutputSymbols == 0
        || program.maximumOutputSymbols > maximumContextFreeOutputSymbols)
    {
        return result;
    }
    for (const auto& rule : program.rules)
    {
        if (rule.alternativeCount == 0
            || rule.alternativeCount > maximumContextFreeAlternativesPerRule
            || rule.firstAlternative > program.alternatives.size()
            || rule.alternativeCount
                > program.alternatives.size() - rule.firstAlternative)
        {
            return result;
        }
    }
    for (const auto& alternative : program.alternatives)
    {
        if (alternative.symbolCount
                > maximumContextFreeSymbolsPerAlternative
            || alternative.firstSymbol > program.replacementSymbols.size()
            || alternative.symbolCount
                > program.replacementSymbols.size()
                    - alternative.firstSymbol)
        {
            return result;
        }
    }
    const auto capacity =
        static_cast<std::size_t>(program.maximumOutputSymbols);
    if (scratch.current.size() < capacity
        || scratch.next.size() < capacity
        || scratch.output.size() < capacity)
    {
        result.status = ContextFreeGenerateStatus::InsufficientScratch;
        return result;
    }
    const auto current = scratch.current.first(capacity);
    const auto next = scratch.next.first(capacity);
    const auto output = scratch.output.first(capacity);
    if (spansOverlap(current, next)
        || spansOverlap(current, output)
        || spansOverlap(next, output))
    {
        result.status = ContextFreeGenerateStatus::AliasedScratch;
        return result;
    }
    std::size_t currentCount = 0;
    for (const auto symbol : program.startSymbols)
    {
        if (symbol >= program.symbols.size()
            || currentCount >= capacity
            || !consumeOperation(result.operations, stopToken))
        {
            result.status = symbol >= program.symbols.size()
                ? ContextFreeGenerateStatus::InvalidPreparedProgram
                : (currentCount >= capacity
                    ? ContextFreeGenerateStatus::OutputLimitExceeded
                    : (stopToken.stop_requested()
                        ? ContextFreeGenerateStatus::Cancelled
                        : ContextFreeGenerateStatus::OperationLimitExceeded));
            return result;
        }
        scratch.current[currentCount++] = symbol;
    }

    auto currentBuffer = current;
    auto nextBuffer = next;
    auto randomState = program.seed;
    while (true)
    {
        if (stopToken.stop_requested())
        {
            result.status = ContextFreeGenerateStatus::Cancelled;
            return result;
        }
        bool complete = true;
        for (std::size_t index = 0; index < currentCount; ++index)
        {
            const auto symbol = currentBuffer[index];
            if (symbol >= program.terminalFlags.size())
                return result;
            complete = complete && program.terminalFlags[symbol] != 0;
        }
        if (complete)
        {
            for (std::size_t index = 0; index < currentCount; ++index)
            {
                if (!consumeOperation(result.operations, stopToken))
                {
                    result.status = stopToken.stop_requested()
                        ? ContextFreeGenerateStatus::Cancelled
                        : ContextFreeGenerateStatus::OperationLimitExceeded;
                    return result;
                }
                output[index] = currentBuffer[index];
            }
            result.status = ContextFreeGenerateStatus::Succeeded;
            result.outputCount = currentCount;
            return result;
        }
        if (result.rewritePasses >= program.maximumRewritePasses)
        {
            result.status = ContextFreeGenerateStatus::RewriteLimitReached;
            return result;
        }
        std::size_t nextCount = 0;
        for (std::size_t index = 0; index < currentCount; ++index)
        {
            if (!consumeOperation(result.operations, stopToken))
            {
                result.status = stopToken.stop_requested()
                    ? ContextFreeGenerateStatus::Cancelled
                    : ContextFreeGenerateStatus::OperationLimitExceeded;
                return result;
            }
            const auto symbol = currentBuffer[index];
            if (symbol >= program.symbols.size())
                return result;
            if (program.terminalFlags[symbol] != 0)
            {
                if (nextCount >= capacity)
                {
                    result.status = ContextFreeGenerateStatus::OutputLimitExceeded;
                    return result;
                }
                nextBuffer[nextCount++] = symbol;
                continue;
            }
            const auto ruleIndex = program.ruleBySymbol[symbol];
            if (ruleIndex < 0
                || static_cast<std::size_t>(ruleIndex) >= program.rules.size())
            {
                return result;
            }
            const auto& rule = program.rules[static_cast<std::size_t>(ruleIndex)];
            if (rule.symbolIndex != symbol || rule.alternativeCount == 0
                || rule.firstAlternative > program.alternatives.size()
                || rule.alternativeCount
                    > program.alternatives.size() - rule.firstAlternative)
            {
                return result;
            }
            const auto alternativeOffset = boundedRandom(
                randomState,
                rule.alternativeCount,
                result.operations,
                stopToken);
            if (!alternativeOffset.has_value())
            {
                result.status = stopToken.stop_requested()
                    ? ContextFreeGenerateStatus::Cancelled
                    : ContextFreeGenerateStatus::OperationLimitExceeded;
                return result;
            }
            const auto& alternative = program.alternatives[
                rule.firstAlternative + *alternativeOffset];
            if (alternative.firstSymbol > program.replacementSymbols.size()
                || alternative.symbolCount
                    > program.replacementSymbols.size() - alternative.firstSymbol)
            {
                return result;
            }
            for (std::size_t replacement = 0;
                 replacement < alternative.symbolCount;
                 ++replacement)
            {
                if (nextCount >= capacity)
                {
                    result.status = ContextFreeGenerateStatus::OutputLimitExceeded;
                    return result;
                }
                if (!consumeOperation(result.operations, stopToken))
                {
                    result.status = stopToken.stop_requested()
                        ? ContextFreeGenerateStatus::Cancelled
                        : ContextFreeGenerateStatus::OperationLimitExceeded;
                    return result;
                }
                const auto replacementSymbol = program.replacementSymbols[
                    alternative.firstSymbol + replacement];
                if (replacementSymbol >= program.symbols.size())
                    return result;
                nextBuffer[nextCount++] = replacementSymbol;
            }
        }
        ++result.rewritePasses;
        currentCount = nextCount;
        std::swap(currentBuffer, nextBuffer);
    }
}

std::span<const std::uint16_t> generatedContextFreeSymbolIndices(
    const ContextFreeGeneratorScratch& scratch,
    const ContextFreeGenerateResult& result) noexcept
{
    if (!result.ok() || result.outputCount > scratch.output.size())
        return {};
    return scratch.output.first(result.outputCount);
}

std::uint64_t contextFreeNextRandomWord(std::uint64_t& state) noexcept
{
    state += 0x9e3779b97f4a7c15ULL;
    auto value = state;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}
}
