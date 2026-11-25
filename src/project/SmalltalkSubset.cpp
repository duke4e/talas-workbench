#include "talas/project/SmalltalkSubset.h"

#include "talas/core/FromChars.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <chrono>
#include <complex>
#include <ctime>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <math.h>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <new>
#include <numbers>
#include <numeric>
#include <random>
#include <ranges>
#include <set>
#include <utility>

namespace talas::project::smalltalk
{
using talas::expr::SourceRange;
using talas::expr::Unit;

namespace
{
// Tokens.
enum class TokenKind : std::uint8_t
{
    End,
    Identifier,
    Keyword,
    Number,
    String,
    Symbol,
    Binary,
    Assign,
    Period,
    LeftParen,
    RightParen,
    LeftBracket,
    RightBracket,
    Colon,
    Bar,
    Caret,
    // An Event Value, !Name, and a variable, ?name.
    EventValue,
    TalasVariable,
    // #( opens a literal array; { and } enclose an element computed as the
    // script runs (#( {Note durationInBeats: 2 ...} ... )).
    ArrayStart,
    LeftBrace,
    RightBrace,
    // ; cascades another message to the same receiver.
    Semicolon,
};

struct Token
{
    TokenKind kind = TokenKind::End;
    std::string text;
    double number = 0.0;
    // An integer's digits in its radix, which an exact number keeps
    //; no radix for a floating point number.
    std::string integerDigits;
    unsigned radix = 0;
    SourceRange range;
};

// The exact number a plain number holds; none for a floating point one or
// one with a unit.
[[nodiscard]] const ExactNumber* exactOf(const Value& value) noexcept
{
    return value.kind == Value::Kind::Number && value.unit == talas::expr::Unit::None ? value.exact.get() : nullptr;
}

// A number literal's value: an integer exact, others floating point.
[[nodiscard]] Value numberLiteral(const Token& token)
{
    if (token.radix != 0)
    {
        std::string_view digits = token.integerDigits;
        const auto negative = digits.starts_with('-');
        if (negative)
            digits.remove_prefix(1);
        if (auto integer = LargeInteger::fromDigits(digits, token.radix))
            return Value::of(ExactNumber::integer(negative ? integer->negated() : std::move(*integer)));
    }
    return Value::of(token.number);
}

[[nodiscard]] bool isLetter(char value) noexcept
{
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || value == '_';
}

[[nodiscard]] bool isDigit(char value) noexcept
{
    return value >= '0' && value <= '9';
}

[[nodiscard]] bool isBinaryCharacter(char value) noexcept
{
    return std::string_view{"+-*/\\<>=~@%&?,"}.find(value) != std::string_view::npos;
}

struct LexResult
{
    std::vector<Token> tokens;
    std::optional<Diagnostic> diagnostic;
};

[[nodiscard]] LexResult lex(std::string_view source)
{
    LexResult result;
    std::size_t offset = 0;
    const auto fail = [&](std::string message, std::size_t begin) {
        result.diagnostic = Diagnostic{std::move(message), {begin, std::max<std::size_t>(1, offset - begin)}};
    };
    // Whether each open bracket holds a literal array's elements: #( and
    // the parentheses inside it, not their {expressions}.
    std::vector<bool> literalArrays;
    // A minus sign starts a negative literal where no operand precedes it,
    // and always among a literal array's elements, as #(0 7 -12).
    const auto operandBefore = [&] {
        if (!literalArrays.empty() && literalArrays.back())
            return false;
        if (result.tokens.empty())
            return false;
        switch (result.tokens.back().kind)
        {
        case TokenKind::Identifier:
        case TokenKind::Number:
        case TokenKind::String:
        case TokenKind::Symbol:
        case TokenKind::RightParen:
        case TokenKind::RightBracket:
        case TokenKind::EventValue:
        case TokenKind::TalasVariable:
        case TokenKind::RightBrace:
            return true;
        default:
            return false;
        }
    };
    while (offset < source.size())
    {
        const auto character = source[offset];
        if (character == ' ' || character == '\t' || character == '\r' || character == '\n')
        {
            ++offset;
            continue;
        }
        const auto begin = offset;
        if (character == '"')
        {
            const auto close = source.find('"', offset + 1);
            if (close == std::string_view::npos)
            {
                offset = source.size();
                fail("A comment is not closed", begin);
                return result;
            }
            offset = close + 1;
            continue;
        }
        Token token;
        if (isLetter(character))
        {
            while (offset < source.size() && (isLetter(source[offset]) || isDigit(source[offset])))
                ++offset;
            token.text.assign(source.substr(begin, offset - begin));
            token.kind = TokenKind::Identifier;
            if (offset < source.size() && source[offset] == ':'
                && (offset + 1 >= source.size() || source[offset + 1] != '='))
            {
                ++offset;
                token.text += ':';
                token.kind = TokenKind::Keyword;
            }
        }
        else if (isDigit(character)
            || (character == '-' && offset + 1 < source.size() && isDigit(source[offset + 1])
                && !operandBefore()))
        {
            const auto negative = character == '-';
            if (negative)
                ++offset;
            const auto digits = offset;
            while (offset < source.size() && isDigit(source[offset]))
                ++offset;
            double value = 0.0;
            if (offset < source.size() && source[offset] == 'r' && offset + 1 < source.size())
            {
                // A radix number, as 16r1F.
                int radix = 0;
                std::from_chars(source.data() + digits, source.data() + offset, radix);
                ++offset;
                // Letter digits in either case, and a fraction after a radix
                // point, as 16rfff and 2r10.1.
                const auto radixDigit = [&](char digit) {
                    const auto value = isDigit(digit) ? digit - '0'
                        : digit >= 'A' && digit <= 'Z' ? digit - 'A' + 10
                        : digit >= 'a' && digit <= 'z' ? digit - 'a' + 10
                                                       : 99;
                    return value < radix ? value : -1;
                };
                const auto start = offset;
                double integerPart = 0.0;
                while (offset < source.size() && radix >= 2 && radix <= 36 && radixDigit(source[offset]) >= 0)
                    integerPart = integerPart * radix + radixDigit(source[offset++]);
                if (offset == start || radix < 2 || radix > 36
                    || (offset < source.size() && (isDigit(source[offset]) || isLetter(source[offset]))))
                {
                    fail("A radix number is malformed", begin);
                    return result;
                }
                value = integerPart;
                token.integerDigits = std::string{source.substr(start, offset - start)};
                token.radix = static_cast<unsigned>(radix);
                if (offset + 1 < source.size() && source[offset] == '.' && radixDigit(source[offset + 1]) >= 0)
                {
                    token.radix = 0;
                    ++offset;
                    auto scale = 1.0 / radix;
                    while (offset < source.size() && radixDigit(source[offset]) >= 0)
                    {
                        value += radixDigit(source[offset++]) * scale;
                        scale /= radix;
                    }
                }
            }
            else
            {
                auto integral = true;
                if (offset + 1 < source.size() && source[offset] == '.' && isDigit(source[offset + 1]))
                {
                    integral = false;
                    ++offset;
                    while (offset < source.size() && isDigit(source[offset]))
                        ++offset;
                }
                if (offset < source.size() && (source[offset] == 'e' || source[offset] == 'd')
                    && offset + 1 < source.size()
                    && (isDigit(source[offset + 1])
                        || (source[offset + 1] == '-' && offset + 2 < source.size()
                            && isDigit(source[offset + 2]))))
                {
                    std::string text{source.substr(digits, offset - digits)};
                    ++offset;
                    const auto exponent = offset;
                    if (source[offset] == '-')
                        ++offset;
                    while (offset < source.size() && isDigit(source[offset]))
                        ++offset;
                    text += 'e';
                    text.append(source.substr(exponent, offset - exponent));
                    talas::fromChars(text.data(), text.data() + text.size(), value);
                }
                else
                {
                    talas::fromChars(source.data() + digits, source.data() + offset, value);
                    if (integral)
                    {
                        token.integerDigits = std::string{source.substr(digits, offset - digits)};
                        token.radix = 10;
                    }
                    // 1.3d is a double, where 4 d, apart,
                    // is a pitch.
                    if (offset < source.size() && source[offset] == 'd'
                        && (offset + 1 == source.size() || !isLetter(source[offset + 1])))
                    {
                        token.radix = 0;
                        ++offset;
                    }
                }
            }
            token.kind = TokenKind::Number;
            token.number = negative ? -value : value;
            if (negative && token.radix != 0)
                token.integerDigits.insert(token.integerDigits.begin(), '-');
            if (!std::isfinite(token.number))
            {
                fail("A number is not finite", begin);
                return result;
            }
        }
        else if (character == '$' && offset + 1 < source.size())
        {
            // A character, as $T, a string of one.
            token.kind = TokenKind::String;
            token.text = std::string(1, source[offset + 1]);
            offset += 2;
        }
        else if (character == '\'' || (character == '#' && offset + 1 < source.size() && source[offset + 1] == '\''))
        {
            // A Symbol may be quoted as a string is, #'.' (Smalltalk-80).
            const auto quoted = character == '#';
            offset += quoted ? 2 : 1;
            while (true)
            {
                if (offset >= source.size())
                {
                    fail(quoted ? "A symbol is not closed" : "A string is not closed", begin);
                    return result;
                }
                if (source[offset] == '\'')
                {
                    if (offset + 1 < source.size() && source[offset + 1] == '\'')
                    {
                        token.text += '\'';
                        offset += 2;
                        continue;
                    }
                    ++offset;
                    break;
                }
                token.text += source[offset++];
            }
            token.kind = quoted ? TokenKind::Symbol : TokenKind::String;
        }
        else if (character == '#' && offset + 1 < source.size() && source[offset + 1] == '(')
        {
            offset += 2;
            token.kind = TokenKind::ArrayStart;
        }
        else if (character == '{' || character == '}')
        {
            ++offset;
            token.kind = character == '{' ? TokenKind::LeftBrace : TokenKind::RightBrace;
        }
        else if (character == '#')
        {
            ++offset;
            const auto start = offset;
            // A binary selector's Symbol, as #+ (Smalltalk-80).
            const auto binary = offset < source.size() && isBinaryCharacter(source[offset]);
            while (offset < source.size()
                && (binary ? isBinaryCharacter(source[offset])
                           : isLetter(source[offset]) || isDigit(source[offset]) || source[offset] == ':'))
            {
                ++offset;
            }
            if (offset == start)
            {
                fail("A symbol has no name", begin);
                return result;
            }
            token.kind = TokenKind::Symbol;
            token.text.assign(source.substr(start, offset - start));
        }
        else if ((character == '!' || character == '?') && offset + 1 < source.size()
            && isLetter(source[offset + 1]))
        {
            ++offset;
            while (offset < source.size() && (isLetter(source[offset]) || isDigit(source[offset])))
                ++offset;
            token.kind = character == '!' ? TokenKind::EventValue : TokenKind::TalasVariable;
            token.text.assign(source.substr(begin + 1, offset - begin - 1));
        }
        else if (character == ':' && offset + 1 < source.size() && source[offset + 1] == '=')
        {
            offset += 2;
            token.kind = TokenKind::Assign;
        }
        else if (character == ':')
        {
            ++offset;
            token.kind = TokenKind::Colon;
        }
        else if (character == '.')
        {
            ++offset;
            token.kind = TokenKind::Period;
        }
        else if (character == ';')
        {
            ++offset;
            token.kind = TokenKind::Semicolon;
            token.text = ";";
        }
        else if (character == '(' || character == ')' || character == '[' || character == ']'
            || character == '|' || character == '^')
        {
            ++offset;
            token.kind = character == '(' ? TokenKind::LeftParen
                : character == ')'        ? TokenKind::RightParen
                : character == '['        ? TokenKind::LeftBracket
                : character == ']'        ? TokenKind::RightBracket
                : character == '|'        ? TokenKind::Bar
                                          : TokenKind::Caret;
            token.text.assign(1, character);
        }
        else if (isBinaryCharacter(character))
        {
            ++offset;
            if (offset < source.size() && isBinaryCharacter(source[offset]) && source[offset] != '-')
                ++offset;
            token.kind = TokenKind::Binary;
            token.text.assign(source.substr(begin, offset - begin));
        }
        else
        {
            ++offset;
            fail("Unexpected character in the script", begin);
            return result;
        }
        token.range = {begin, offset - begin};
        switch (token.kind)
        {
        case TokenKind::ArrayStart:
            literalArrays.push_back(true);
            break;
        case TokenKind::LeftParen:
            literalArrays.push_back(!literalArrays.empty() && literalArrays.back());
            break;
        case TokenKind::LeftBracket:
        case TokenKind::LeftBrace:
            literalArrays.push_back(false);
            break;
        case TokenKind::RightParen:
        case TokenKind::RightBracket:
        case TokenKind::RightBrace:
            if (!literalArrays.empty())
                literalArrays.pop_back();
            break;
        default:
            break;
        }
        result.tokens.push_back(std::move(token));
    }
    Token end;
    end.range = {source.size(), 0};
    result.tokens.push_back(std::move(end));
    return result;
}
}

// The parsed program: expression nodes in one arena, with the body of the
// program and of each block as a list of temporaries and statements.
struct Body
{
    std::vector<std::string> parameters;
    std::vector<std::string> temporaries;
    std::vector<std::size_t> statements;
};

struct Node
{
    enum class Kind : std::uint8_t
    {
        Literal,
        Variable,
        Assign,
        Message,
        Block,
        Return,
        // An array of its operands' values.
        Array,
        // A cascade: its first operand is the receiver, evaluated once, and
        // the others the messages sent to it, the last answering; each
        // names the receiver through a CascadeReceiver.
        Cascade,
        CascadeReceiver,
    };

    Kind kind = Kind::Literal;
    Value literal;
    std::string name;
    std::string selector;
    std::vector<std::size_t> operands;
    std::size_t body = 0;
    SourceRange range;
    // A keyword message's keywords whose argument touches the colon, as
    // freq:500, each with its range.
    std::vector<std::pair<std::string, SourceRange>> touchingKeywords;
};

class Program
{
public:
    std::vector<Node> nodes;
    std::vector<Body> bodies;
};

namespace
{
class Parser
{
public:
    explicit Parser(std::vector<Token> tokens)
        : tokens_(std::move(tokens))
    {
    }

    std::shared_ptr<Program> parseProgram(std::optional<Diagnostic>& diagnostic)
    {
        auto program = std::make_shared<Program>();
        program_ = program.get();
        program_->bodies.emplace_back();
        if (!parseBody(0, TokenKind::End))
        {
            diagnostic = diagnostic_;
            return nullptr;
        }
        return program;
    }

private:
    [[nodiscard]] const Token& current() const noexcept { return tokens_[position_]; }
    void advance() noexcept
    {
        if (position_ + 1 < tokens_.size())
            ++position_;
    }
    // Steps past a keyword, noting in `node` an argument that touches its
    // colon.
    void advancePastKeyword(Node& node)
    {
        const auto keyword = current();
        advance();
        if (current().kind != TokenKind::End && current().range.offset == keyword.range.offset + keyword.range.length)
            node.touchingKeywords.emplace_back(keyword.text, keyword.range);
    }
    bool fail(std::string message)
    {
        if (!diagnostic_.has_value())
            diagnostic_ = Diagnostic{std::move(message), current().range};
        return false;
    }

    std::size_t add(Node node)
    {
        program_->nodes.push_back(std::move(node));
        return program_->nodes.size() - 1;
    }

    bool parseBody(std::size_t bodyIndex, TokenKind terminator)
    {
        if (current().kind == TokenKind::Bar)
        {
            advance();
            while (current().kind == TokenKind::Identifier)
            {
                program_->bodies[bodyIndex].temporaries.push_back(current().text);
                advance();
            }
            if (current().kind != TokenKind::Bar)
                return fail("Temporaries end with |");
            advance();
        }
        while (current().kind != terminator)
        {
            if (current().kind == TokenKind::Period)
            {
                advance();
                continue;
            }
            std::size_t statement = 0;
            if (current().kind == TokenKind::Caret)
            {
                const auto range = current().range;
                advance();
                std::size_t value = 0;
                if (!parseExpression(value))
                    return false;
                Node node;
                node.kind = Node::Kind::Return;
                node.operands = {value};
                node.range = range;
                statement = add(std::move(node));
            }
            else if (!parseExpression(statement))
            {
                return false;
            }
            program_->bodies[bodyIndex].statements.push_back(statement);
            if (current().kind == TokenKind::Period)
                advance();
            else if (current().kind != terminator)
                return fail("Statements are separated by periods");
        }
        return true;
    }

    bool parseExpression(std::size_t& result)
    {
        if (++depth_ > maximumDepth)
            return fail("The script nests too deeply");
        const auto leave = [this](bool ok) {
            --depth_;
            return ok;
        };
        if (current().kind == TokenKind::Identifier
            && position_ + 1 < tokens_.size() && tokens_[position_ + 1].kind == TokenKind::Assign)
        {
            Node node;
            node.kind = Node::Kind::Assign;
            node.name = current().text;
            node.range = current().range;
            advance();
            advance();
            std::size_t value = 0;
            if (!parseExpression(value))
                return leave(false);
            node.operands = {value};
            result = add(std::move(node));
            return leave(true);
        }
        std::size_t receiver = 0;
        if (!parseBinary(receiver))
            return leave(false);
        if (current().kind != TokenKind::Keyword)
        {
            result = receiver;
            return leave(current().kind != TokenKind::Semicolon || parseCascade(result));
        }
        Node node;
        node.kind = Node::Kind::Message;
        node.range = current().range;
        node.operands.push_back(receiver);
        while (current().kind == TokenKind::Keyword)
        {
            node.selector += current().text;
            advancePastKeyword(node);
            std::size_t argument = 0;
            if (!parseBinary(argument))
                return leave(false);
            node.operands.push_back(argument);
        }
        result = add(std::move(node));
        return leave(current().kind != TokenKind::Semicolon || parseCascade(result));
    }

    // receiver message; message; ... sends each message to the receiver of
    // the first one, answering the last (Smalltalk's cascade).
    bool parseCascade(std::size_t& result)
    {
        if (program_->nodes[result].kind != Node::Kind::Message)
            return fail("A cascade follows a message");
        Node cascade;
        cascade.kind = Node::Kind::Cascade;
        cascade.range = program_->nodes[result].range;
        cascade.operands.push_back(program_->nodes[result].operands.front());
        Node placeholder;
        placeholder.kind = Node::Kind::CascadeReceiver;
        placeholder.range = cascade.range;
        const auto receiver = add(std::move(placeholder));
        program_->nodes[result].operands.front() = receiver;
        cascade.operands.push_back(result);
        while (current().kind == TokenKind::Semicolon)
        {
            advance();
            auto message = receiver;
            if (!parseMessagesTo(message))
                return false;
            if (message == receiver)
                return fail("Each part of a cascade is a message");
            cascade.operands.push_back(message);
        }
        result = add(std::move(cascade));
        return true;
    }

    // The unary, binary and keyword messages that follow, sent to result.
    bool parseMessagesTo(std::size_t& result)
    {
        while (current().kind == TokenKind::Identifier)
        {
            Node node;
            node.kind = Node::Kind::Message;
            node.selector = current().text;
            node.range = current().range;
            node.operands = {result};
            advance();
            result = add(std::move(node));
        }
        while (current().kind == TokenKind::Binary || current().kind == TokenKind::Bar)
        {
            Node node;
            node.kind = Node::Kind::Message;
            node.selector = current().text;
            node.range = current().range;
            advance();
            std::size_t argument = 0;
            if (!parseUnary(argument))
                return false;
            node.operands = {result, argument};
            result = add(std::move(node));
        }
        if (current().kind == TokenKind::Keyword)
        {
            Node node;
            node.kind = Node::Kind::Message;
            node.range = current().range;
            node.operands.push_back(result);
            while (current().kind == TokenKind::Keyword)
            {
                node.selector += current().text;
                advancePastKeyword(node);
                std::size_t argument = 0;
                if (!parseBinary(argument))
                    return false;
                node.operands.push_back(argument);
            }
            result = add(std::move(node));
        }
        return true;
    }

    bool parseBinary(std::size_t& result)
    {
        if (!parseUnary(result))
            return false;
        while (current().kind == TokenKind::Binary || current().kind == TokenKind::Bar)
        {
            Node node;
            node.kind = Node::Kind::Message;
            node.selector = current().text;
            node.range = current().range;
            advance();
            std::size_t argument = 0;
            if (!parseUnary(argument))
                return false;
            node.operands = {result, argument};
            result = add(std::move(node));
        }
        return true;
    }

    bool parseUnary(std::size_t& result)
    {
        if (!parsePrimary(result))
            return false;
        while (current().kind == TokenKind::Identifier)
        {
            Node node;
            node.kind = Node::Kind::Message;
            node.selector = current().text;
            node.range = current().range;
            node.operands = {result};
            advance();
            result = add(std::move(node));
        }
        return true;
    }

    bool parsePrimary(std::size_t& result)
    {
        const auto& token = current();
        Node node;
        node.range = token.range;
        switch (token.kind)
        {
        case TokenKind::Number:
            node.kind = Node::Kind::Literal;
            node.literal = numberLiteral(token);
            advance();
            break;
        case TokenKind::String:
            node.kind = Node::Kind::Literal;
            node.literal = Value::string(token.text);
            advance();
            break;
        case TokenKind::Symbol:
            node.kind = Node::Kind::Literal;
            node.literal.kind = Value::Kind::Symbol;
            node.literal.text = token.text;
            advance();
            break;
        case TokenKind::Identifier:
            node.kind = Node::Kind::Variable;
            node.name = token.text;
            advance();
            break;
        case TokenKind::EventValue:
            // An Event Value is an expression the Sound evaluates as it
            // plays (delay: (!Delay * (i inverse))).
            node.kind = Node::Kind::Literal;
            node.literal = Value::expression("!" + token.text);
            advance();
            break;
        case TokenKind::TalasVariable:
            node.kind = Node::Kind::Variable;
            node.name = "?" + token.text;
            advance();
            break;
        case TokenKind::LeftParen:
        {
            advance();
            if (!parseExpression(result))
                return false;
            if (current().kind != TokenKind::RightParen)
                return fail("A parenthesis is not closed");
            advance();
            return true;
        }
        case TokenKind::ArrayStart:
            advance();
            return parseLiteralArray(result, node.range);
        case TokenKind::LeftBrace:
        {
            // {a. b. c} is an Array of the statements' values.
            advance();
            node.kind = Node::Kind::Array;
            while (current().kind != TokenKind::RightBrace)
            {
                if (current().kind == TokenKind::Period)
                {
                    advance();
                    continue;
                }
                std::size_t element = 0;
                if (!parseExpression(element))
                    return false;
                node.operands.push_back(element);
                if (current().kind != TokenKind::Period && current().kind != TokenKind::RightBrace)
                    return fail("Brace elements are separated by periods");
            }
            advance();
            break;
        }
        case TokenKind::LeftBracket:
        {
            advance();
            const auto bodyIndex = program_->bodies.size();
            program_->bodies.emplace_back();
            while (current().kind == TokenKind::Colon)
            {
                advance();
                if (current().kind != TokenKind::Identifier)
                    return fail("A block parameter needs a name");
                program_->bodies[bodyIndex].parameters.push_back(current().text);
                advance();
            }
            if (!program_->bodies[bodyIndex].parameters.empty())
            {
                if (current().kind == TokenKind::Bar)
                    advance();
                else if (current().kind != TokenKind::RightBracket)
                    return fail("Block parameters end with |");
            }
            if (!parseBody(bodyIndex, TokenKind::RightBracket))
                return false;
            if (current().kind != TokenKind::RightBracket)
                return fail("A block is not closed");
            advance();
            node.kind = Node::Kind::Block;
            node.body = bodyIndex;
            break;
        }
        default:
            return fail("Expected a value");
        }
        result = add(std::move(node));
        return true;
    }

    // A literal array's elements: numbers, strings, symbols, bare words as
    // symbols, nested arrays in parentheses, and {expressions}.
    bool parseLiteralArray(std::size_t& result, SourceRange range)
    {
        if (++depth_ > maximumDepth)
            return fail("The script nests too deeply");
        Node node;
        node.kind = Node::Kind::Array;
        node.range = range;
        while (current().kind != TokenKind::RightParen)
        {
            const auto& token = current();
            Node element;
            element.kind = Node::Kind::Literal;
            element.range = token.range;
            switch (token.kind)
            {
            case TokenKind::Number:
                element.literal = numberLiteral(token);
                advance();
                break;
            case TokenKind::String:
                element.literal = Value::string(token.text);
                advance();
                break;
            case TokenKind::Symbol:
            case TokenKind::Identifier:
            case TokenKind::Keyword:
            case TokenKind::Binary:
                if (token.text == "true" || token.text == "false")
                    element.literal = Value::of(token.text == "true");
                else if (token.text == "nil")
                    element.literal = Value::nil();
                else
                {
                    element.literal.kind = Value::Kind::Symbol;
                    element.literal.text = token.text;
                }
                advance();
                break;
            case TokenKind::LeftParen:
            case TokenKind::ArrayStart:
            {
                const auto nested = token.range;
                advance();
                std::size_t inner = 0;
                if (!parseLiteralArray(inner, nested))
                    return false;
                node.operands.push_back(inner);
                continue;
            }
            case TokenKind::LeftBrace:
            {
                advance();
                std::size_t computed = 0;
                if (!parseExpression(computed))
                    return false;
                if (current().kind != TokenKind::RightBrace)
                    return fail("A braced element is not closed");
                advance();
                node.operands.push_back(computed);
                continue;
            }
            case TokenKind::End:
                return fail("A literal array is not closed");
            default:
                return fail("A literal array holds literals and {expressions}");
            }
            node.operands.push_back(add(std::move(element)));
        }
        advance();
        --depth_;
        result = add(std::move(node));
        return true;
    }

    std::vector<Token> tokens_;
    std::size_t position_ = 0;
    std::size_t depth_ = 0;
    Program* program_ = nullptr;
    std::optional<Diagnostic> diagnostic_;
};
}

std::vector<std::string> unreadTemporaries(const Program& program)
{
    std::set<std::string, std::less<>> read;
    std::set<std::string, std::less<>> assigned;
    for (const auto& node : program.nodes)
    {
        if (node.kind == Node::Kind::Variable)
            read.insert(node.name);
        else if (node.kind == Node::Kind::Assign)
            assigned.insert(node.name);
    }
    std::vector<std::string> unread;
    for (const auto& body : program.bodies)
        for (const auto& name : body.temporaries)
            if (assigned.contains(name) && !read.contains(name) && std::ranges::find(unread, name) == unread.end())
                unread.push_back(name);
    return unread;
}

std::map<std::string, std::set<std::string>> eventKeywords(const Program& program)
{
    std::map<std::string, std::set<std::string>> keywords;
    for (const auto& node : program.nodes)
    {
        if (node.kind != Node::Kind::Message || !node.selector.starts_with("start:") || node.operands.empty()
            || node.operands.front() >= program.nodes.size())
            continue;
        const auto& receiver = program.nodes[node.operands.front()];
        if (receiver.kind != Node::Kind::Variable)
            continue;
        auto& names = keywords[receiver.name];
        std::size_t from = 0;
        for (auto colon = node.selector.find(':'); colon != std::string::npos; colon = node.selector.find(':', from))
        {
            const auto keyword = node.selector.substr(from, colon - from);
            if (keyword != "start" && !keyword.empty())
                names.insert(keyword);
            from = colon + 1;
        }
    }
    return keywords;
}

std::optional<Diagnostic> eventValueTouchingItsColon(const Program& program)
{
    for (const auto& node : program.nodes)
    {
        if (node.kind != Node::Kind::Message || !node.selector.starts_with("start:") || node.operands.empty()
            || node.operands.front() >= program.nodes.size() || node.touchingKeywords.empty()
            || program.nodes[node.operands.front()].kind != Node::Kind::Variable)
            continue;
        const auto& [keyword, range] = node.touchingKeywords.front();
        return Diagnostic{"A space must come between " + keyword
                + " and its value in a Script's event",
            range};
    }
    return std::nullopt;
}

ParseResult parse(std::string_view source)
{
    ParseResult result;
    try
    {
        if (source.size() > maximumSourceBytes)
        {
            result.diagnostic = Diagnostic{"The script exceeds its fixed source limit", {0, source.size()}};
            return result;
        }
        auto lexed = lex(source);
        if (lexed.diagnostic.has_value())
        {
            result.diagnostic = std::move(lexed.diagnostic);
            return result;
        }
        Parser parser(std::move(lexed.tokens));
        result.program = parser.parseProgram(result.diagnostic);
    }
    catch (const std::bad_alloc&)
    {
        result.program.reset();
        result.diagnostic = Diagnostic{"The script could not be parsed within memory", {}};
    }
    return result;
}

namespace
{
// Units by quantity: frequencies convert through hertz, times through
// seconds; the others combine only with themselves.
enum class Quantity : std::uint8_t
{
    None,
    Frequency,
    Time,
    Other,
};

[[nodiscard]] Quantity quantity(Unit unit) noexcept
{
    switch (unit)
    {
    case Unit::None:
        return Quantity::None;
    case Unit::Hertz:
    case Unit::NoteNumber:
    case Unit::BeatsPerMinute:
        return Quantity::Frequency;
    case Unit::Milliseconds:
    case Unit::Seconds:
    case Unit::Microseconds:
    case Unit::Minutes:
    case Unit::Hours:
    case Unit::Days:
        return Quantity::Time;
    default:
        return Quantity::Other;
    }
}

// A time as SMPTE time code, hh:mm:ss.ff at the frames a second given, the
// frame it falls in (25.5 s asSMPTE is 00:00:25.15 at 30).
[[nodiscard]] std::string smpteText(double seconds, unsigned framesPerSecond)
{
    const auto rate = static_cast<long long>(std::max(1U, framesPerSecond));
    const auto frames = static_cast<long long>(std::floor(seconds * static_cast<double>(rate) + 1.0e-9));
    const auto whole = frames / rate;
    std::array<char, 48> text{};
    std::snprintf(text.data(), text.size(), "%02lld:%02lld:%02lld.%02lld", whole / 3600, (whole / 60) % 60, whole % 60,
        frames % rate);
    return text.data();
}

[[nodiscard]] double toBase(double value, Unit unit) noexcept
{
    switch (unit)
    {
    case Unit::NoteNumber:
        return 440.0 * std::exp2((value - 69.0) / 12.0);
    case Unit::BeatsPerMinute:
        return value / 60.0;
    case Unit::Milliseconds:
        return value / 1000.0;
    case Unit::Microseconds:
        return value / 1'000'000.0;
    case Unit::Minutes:
        return value * 60.0;
    case Unit::Hours:
        return value * 3600.0;
    case Unit::Days:
        return value * 86'400.0;
    default:
        return value;
    }
}

[[nodiscard]] double fromBase(double value, Unit unit) noexcept
{
    switch (unit)
    {
    case Unit::NoteNumber:
        return 69.0 + 12.0 * std::log2(value / 440.0);
    case Unit::BeatsPerMinute:
        return value * 60.0;
    case Unit::Milliseconds:
        return value * 1000.0;
    case Unit::Microseconds:
        return value * 1'000'000.0;
    case Unit::Minutes:
        return value / 60.0;
    case Unit::Hours:
        return value / 3600.0;
    case Unit::Days:
        return value / 86'400.0;
    default:
        return value;
    }
}

[[nodiscard]] Unit baseUnit(Quantity value) noexcept
{
    return value == Quantity::Frequency ? Unit::Hertz : value == Quantity::Time ? Unit::Seconds : Unit::None;
}

[[nodiscard]] std::optional<Unit> unitNamed(std::string_view name) noexcept
{
    if (name == "hz")
        return Unit::Hertz;
    if (name == "nn")
        return Unit::NoteNumber;
    if (name == "s")
        return Unit::Seconds;
    if (name == "ms")
        return Unit::Milliseconds;
    if (name == "usec")
        return Unit::Microseconds;
    if (name == "m")
        return Unit::Minutes;
    if (name == "h")
        return Unit::Hours;
    if (name == "days")
        return Unit::Days;
    if (name == "samp" || name == "samples")
        return Unit::Samples;
    // A single beat is written as 1 beat.
    if (name == "beats" || name == "beat")
        return Unit::Beats;
    if (name == "dB" || name == "db")
        return Unit::Decibels;
    // A tempo, as Val's 1 bpm:.
    if (name == "bpm")
        return Unit::BeatsPerMinute;
    return std::nullopt;
}

// Pitch names (4 a is the a above middle c),
// in half steps above c, as letters or solfege syllables.
[[nodiscard]] std::optional<double> pitchNamed(std::string_view name) noexcept
{
    constexpr std::array<std::pair<std::string_view, double>, 16> names{{
        {"c", 0.0}, {"do", 0.0}, {"d", 2.0}, {"re", 2.0}, {"e", 4.0}, {"mi", 4.0}, {"f", 5.0}, {"fa", 5.0},
        {"g", 7.0}, {"so", 7.0}, {"sol", 7.0}, {"a", 9.0}, {"la", 9.0}, {"b", 11.0}, {"si", 11.0}, {"ti", 11.0},
    }};
    const auto found = std::ranges::find(names, name, &std::pair<std::string_view, double>::first);
    if (found == names.end())
        return std::nullopt;
    return found->second;
}

[[nodiscard]] std::string_view unitText(Unit unit) noexcept
{
    switch (unit)
    {
    case Unit::Hertz:
        return "hz";
    case Unit::NoteNumber:
        return "nn";
    case Unit::Seconds:
        return "s";
    case Unit::Milliseconds:
        return "ms";
    case Unit::Microseconds:
        return "usec";
    case Unit::Minutes:
        return "m";
    case Unit::Hours:
        return "h";
    case Unit::Days:
        return "days";
    case Unit::Samples:
        return "samp";
    case Unit::Beats:
        return "beats";
    case Unit::Decibels:
        return "dB";
    case Unit::BeatsPerMinute:
        return "bpm";
    default:
        return {};
    }
}

[[nodiscard]] std::string numberText(double value)
{
    std::array<char, 64> digits{};
    const auto written = std::to_chars(digits.data(), digits.data() + digits.size(), value);
    return std::string(digits.data(), written.ptr);
}

struct Frame
{
    std::map<std::string, Value, std::less<>> variables;
    std::shared_ptr<Frame> parent;
};

struct Closure
{
    std::size_t body = 0;
    std::shared_ptr<Frame> frame;
    // The program the block was written in, which a session may have run
    // before the one now running.
    const Program* program = nullptr;
    // Alive while a Value holds the block.
    std::weak_ptr<const std::vector<Value>> held;
};

struct Failure
{
    std::string message;
    SourceRange range;
};

// recognizes:'s trial of a message stopped before it reaches the host or
// the files: whether the message was understood that far.
struct TrialStop
{
    bool recognized = false;
};

// A non-local return unwinding to the top of the program.
struct Unwind
{
    Value value;
};

class Interpreter final : public Messenger
{
    // The interpreter's own objects, which scripts make and change.
    struct ReadStream
    {
        std::vector<Value> elements;
        std::size_t position = 0;
        bool text = false;
        bool writable = false;
        // The line end cr writes: Character cr, or a file's line end.
        std::string lineEnd = "\r";
        // A file's stream: its path, whether it
        // reads and writes, whether its writes go to the end, and whether
        // it is closed or holds what the file does not yet.
        std::optional<std::string> file;
        bool reads = true;
        bool writes = true;
        bool appending = false;
        bool closed = false;
        bool dirty = false;
    };

    // An OrderedCollection, a Set (each element once), a Bag (any number of
    // each) or a SortedCollection (kept in its block's order, or <=).
    struct Collection
    {
        bool isSet = false;
        std::vector<Value> elements;
        bool bag = false;
        bool sorted = false;
        std::optional<Value> sortBlock;
    };

    // A Dictionary's keys and values, in the order the keys came.
    struct Dictionary
    {
        std::vector<std::pair<Value, Value>> entries;
    };

public:
    Interpreter(const Program& program, Host& host)
        : program_(&program)
        , host_(host)
        , exactNumbers_(host.exactNumbers())
    {
    }

    // The next program a session runs, its counts of steps and depth
    // starting again.
    void begin(const Program& program) noexcept
    {
        program_ = &program;
        steps_ = 0;
        depth_ = 0;
    }

    // A host's message back to the interpreter's own objects.
    std::optional<Value> send(const Value& receiver, std::string_view selector, std::span<const Value> arguments,
        std::string& error) override
    {
        try
        {
            return send(receiver, std::string{selector}, arguments, SourceRange{});
        }
        catch (const Failure& failure)
        {
            error = failure.message;
            return std::nullopt;
        }
    }

    // What a value prints as, as printString answers it.
    std::string printString(const Value& value)
    {
        return printed(value, false, 0);
    }

    Value runBody(std::size_t bodyIndex, std::shared_ptr<Frame> frame, std::span<const Value> arguments = {})
    {
        if (++depth_ > maximumDepth)
            throw Failure{"The script recurses too deeply", {}};
        // Every block run is a step, so that a loop of no messages ends.
        if (++steps_ > maximumSteps)
            throw Failure{"The script exceeds its fixed number of steps", {}};
        const auto& body = program_->bodies[bodyIndex];
        auto local = std::make_shared<Frame>();
        local->parent = std::move(frame);
        for (std::size_t index = 0; index < body.parameters.size(); ++index)
            local->variables[body.parameters[index]] = index < arguments.size() ? arguments[index] : Value::nil();
        for (const auto& name : body.temporaries)
            local->variables[name] = Value::nil();
        Value last;
        for (const auto statement : body.statements)
            last = evaluate(statement, local);
        --depth_;
        return last;
    }

private:
    Value evaluate(std::size_t nodeIndex, const std::shared_ptr<Frame>& frame)
    {
        const auto& node = program_->nodes[nodeIndex];
        switch (node.kind)
        {
        case Node::Kind::Literal:
            if (!exactNumbers_ && node.literal.exact != nullptr)
                return Value::of(node.literal.number);
            return node.literal;
        case Node::Kind::Variable:
            return lookup(node, frame);
        case Node::Kind::Assign:
        {
            auto value = evaluate(node.operands.front(), frame);
            for (auto* scope = frame.get(); scope != nullptr; scope = scope->parent.get())
            {
                const auto found = scope->variables.find(node.name);
                if (found != scope->variables.end())
                {
                    found->second = value;
                    return value;
                }
            }
            if (trial_)
                throw TrialStop{true};
            if (host_.assign(node.name, value))
                return value;
            throw Failure{"Assignment to an undeclared variable " + node.name, node.range};
        }
        case Node::Kind::Block:
            return newBlock(node.body, frame);
        case Node::Kind::Return:
            throw Unwind{evaluate(node.operands.front(), frame)};
        case Node::Kind::Array:
        {
            std::vector<Value> elements;
            elements.reserve(node.operands.size());
            for (const auto operand : node.operands)
                elements.push_back(evaluate(operand, frame));
            return Value::array(std::move(elements));
        }
        case Node::Kind::Cascade:
        {
            cascadeReceivers_.push_back(evaluate(node.operands.front(), frame));
            struct Leave
            {
                std::vector<Value>& receivers;
                ~Leave() { receivers.pop_back(); }
            } leave{cascadeReceivers_};
            Value last;
            for (std::size_t index = 1; index < node.operands.size(); ++index)
                last = evaluate(node.operands[index], frame);
            return last;
        }
        case Node::Kind::CascadeReceiver:
            if (cascadeReceivers_.empty())
                throw Failure{"A cascade's receiver is missing", node.range};
            return cascadeReceivers_.back();
        case Node::Kind::Message:
        {
            const auto receiver = evaluate(node.operands.front(), frame);
            std::vector<Value> arguments;
            arguments.reserve(node.operands.size() - 1);
            for (std::size_t index = 1; index < node.operands.size(); ++index)
                arguments.push_back(evaluate(node.operands[index], frame));
            if (++steps_ > maximumSteps)
                throw Failure{"The script exceeds its fixed number of steps", node.range};
            if (!host_.stackTraces())
                return send(receiver, node.selector, arguments, node.range);
            try
            {
                return send(receiver, node.selector, arguments, node.range);
            }
            catch (const Failure& failure)
            {
                // Allow stack traces: the failure tells
                // each message it was sent within, innermost first.
                auto message = failure.message;
                if (message.find("\nStack trace:") == std::string::npos)
                    message += "\nStack trace:";
                message += "\n  " + node.selector + " (bytes " + std::to_string(node.range.offset) + "-"
                    + std::to_string(node.range.offset + node.range.length) + ")";
                throw Failure{std::move(message), failure.range};
            }
        }
        }
        return {};
    }

    Value lookup(const Node& node, const std::shared_ptr<Frame>& frame)
    {
        if (node.name == "nil")
            return Value::nil();
        if (node.name == "true" || node.name == "false")
            return Value::of(node.name == "true");
        for (auto* scope = frame.get(); scope != nullptr; scope = scope->parent.get())
        {
            const auto found = scope->variables.find(node.name);
            if (found != scope->variables.end())
                return found->second;
        }
        if (auto value = host_.variable(node.name))
            return *value;
        if (node.name == "Random")
            return Value::object(randomClass);
        if (node.name == "Float")
            return Value::object(floatClass);
        if (node.name == "Date")
            return Value::object(dateClass);
        if (node.name == "SamplesFile")
            return Value::object(samplesFileClass);
        if (node.name == "Time")
            return Value::object(timeClass);
        if (node.name == "OneOverF")
            return Value::object(oneOverFClass);
        if (node.name == "MarkovChain")
            return Value::object(markovChainClass);
        if (node.name == "Array")
            return Value::object(arrayClass);
        if (node.name == "ReadStream")
            return Value::object(readStreamClass);
        if (node.name == "BindingList")
            return Value::object(bindingListClass);
        if (node.name == "Point")
            return Value::object(pointClass);
        if (node.name == "XYGraphView")
            return Value::object(xyGraphViewClass);
        if (node.name == "Filename")
            return Value::object(filenameClass);
        if (node.name == "HostDriverInterface")
            return Value::object(hostDriverInterfaceClass);
        if (node.name == "Preferences")
            return Value::object(preferencesClass);
        if (node.name == "Rectangle")
            return Value::object(rectangleClass);
        if (node.name == "OrderedCollection")
            return Value::object(orderedCollectionClass);
        if (node.name == "Set")
            return Value::object(setClass);
        if (node.name == "Bag")
            return Value::object(bagClass);
        if (node.name == "SortedCollection")
            return Value::object(sortedCollectionClass);
        if (node.name == "WriteStream")
            return Value::object(writeStreamClass);
        if (node.name == "ReadWriteStream")
            return Value::object(readWriteStreamClass);
        if (node.name == "Dictionary")
            return Value::object(dictionaryClass);
        if (node.name == "String")
            return Value::object(stringClass);
        // The script itself, which shows values.
        if (node.name == "self")
            return Value::object(scriptSelf);
        // A variable the host does not know is set from outside the
        // script: it reaches a field as @name.
        if (node.name.starts_with('?'))
            return Value::expression("@" + node.name.substr(1));
        // The other classes a script names, as Number or Collection.
        if (superclasses().contains(node.name) || node.name == "Object")
            return classValue(node.name);
        throw Failure{"Unknown name " + node.name, node.range};
    }

    // A block, in the slot of one no Value holds any more where there is
    // one, so a Session that runs for hours, as a Tool's does, keeps only
    // the blocks it can still run.
    Value newBlock(std::size_t body, const std::shared_ptr<Frame>& frame)
    {
        if (freeClosures_.empty() && closures_.size() >= sweepClosuresAt_)
        {
            for (std::size_t index = 0; index < closures_.size(); ++index)
            {
                if (closures_[index].program != nullptr && closures_[index].held.expired())
                {
                    closures_[index] = {};
                    freeClosures_.push_back(index);
                }
            }
            sweepClosuresAt_ = std::max<std::size_t>(1024, 2 * (closures_.size() - freeClosures_.size()));
        }
        auto held = std::make_shared<const std::vector<Value>>();
        Value value;
        value.kind = Value::Kind::Block;
        value.elements = held;
        Closure closure{body, frame, program_, held};
        if (freeClosures_.empty())
        {
            closures_.push_back(std::move(closure));
            value.index = closures_.size() - 1;
            return value;
        }
        value.index = freeClosures_.back();
        freeClosures_.pop_back();
        closures_[value.index] = std::move(closure);
        return value;
    }

    Value callBlock(const Value& block, std::span<const Value> arguments, const SourceRange& range)
    {
        if (block.kind != Value::Kind::Block)
            throw Failure{"Expected a block", range};
        const auto closure = closures_[block.index];
        if (closure.program->bodies[closure.body].parameters.size() != arguments.size())
            throw Failure{"A block takes another number of arguments", range};
        // A block runs in its own program's nodes.
        struct Within
        {
            const Program*& running;
            const Program* previous;
            ~Within() { running = previous; }
        } within{program_, program_};
        program_ = closure.program;
        return runBody(closure.body, closure.frame, arguments);
    }

    [[nodiscard]] bool truth(const Value& value, const SourceRange& range) const
    {
        if (value.kind != Value::Kind::Boolean)
            throw Failure{"Expected true or false", range};
        return value.boolean;
    }

    // The message tried with nil arguments, the interpreter's objects kept
    // as they were and nothing sent to the host or written: any answer, or
    // any failure but the receiver's not understanding that message, means
    // it is understood. A block answers for the messages blocks take.
    bool recognizes(const Value& receiver, const std::string& selector, const SourceRange& range)
    {
        if (selector.empty() || trial_)
            return false;
        if (receiver.kind == Value::Kind::Block)
        {
            static constexpr std::array<std::string_view, 16> blockMessages{"value", "value:", "value:value:",
                "value:value:value:", "value:value:value:value:", "valueWithArguments:", "numArgs", "whileTrue:",
                "whileFalse:", "whileTrue", "whileFalse", "repeat", "ensure:", "ifCurtailed:", "on:do:", "fork"};
            return std::ranges::find(blockMessages, std::string_view{selector}) != blockMessages.end();
        }
        const auto binary = std::isalpha(static_cast<unsigned char>(selector.front())) == 0;
        const auto arity = binary ? std::size_t{1} : static_cast<std::size_t>(std::ranges::count(selector, ':'));
        const std::vector<Value> nils(arity, Value::nil());
        auto randoms = randoms_;
        auto samplesFiles = samplesFiles_;
        auto oneOverFs = oneOverFs_;
        auto markovChains = markovChains_;
        auto streams = streams_;
        auto collections = collections_;
        auto dictionaries = dictionaries_;
        auto randomsHeld = randomsHeld_;
        auto oneOverFsHeld = oneOverFsHeld_;
        auto samplesFilesHeld = samplesFilesHeld_;
        auto streamsHeld = streamsHeld_;
        auto collectionsHeld = collectionsHeld_;
        auto dictionariesHeld = dictionariesHeld_;
        const auto cascadeReceivers = cascadeReceivers_;
        const auto markovTrainingElements = markovTrainingElements_;
        const auto unlikeKeys = unlikeKeys_;
        const auto steps = steps_;
        const auto depth = depth_;
        trial_ = true;
        bool understood = true;
        try
        {
            (void) send(receiver, selector, nils, range);
        }
        catch (const TrialStop& stop)
        {
            understood = stop.recognized;
        }
        catch (const Failure& failure)
        {
            const auto unknown = "does not understand " + selector;
            understood = !(failure.message.size() >= unknown.size()
                && failure.message.compare(failure.message.size() - unknown.size(), unknown.size(), unknown) == 0);
        }
        catch (...)
        {
        }
        trial_ = false;
        randoms_ = std::move(randoms);
        samplesFiles_ = std::move(samplesFiles);
        oneOverFs_ = std::move(oneOverFs);
        markovChains_ = std::move(markovChains);
        streams_ = std::move(streams);
        collections_ = std::move(collections);
        dictionaries_ = std::move(dictionaries);
        randomsHeld_ = std::move(randomsHeld);
        oneOverFsHeld_ = std::move(oneOverFsHeld);
        samplesFilesHeld_ = std::move(samplesFilesHeld);
        streamsHeld_ = std::move(streamsHeld);
        collectionsHeld_ = std::move(collectionsHeld);
        dictionariesHeld_ = std::move(dictionariesHeld);
        cascadeReceivers_ = cascadeReceivers;
        markovTrainingElements_ = markovTrainingElements;
        unlikeKeys_ = unlikeKeys;
        steps_ = steps;
        depth_ = depth;
        return understood;
    }

    Value send(const Value& receiver, const std::string& selector, std::span<const Value> arguments,
        const SourceRange& range)
    {
        if (selector == "isNil")
            return Value::of(receiver.kind == Value::Kind::Nil);
        if (selector == "yourself")
            return receiver;
        // The receiver's class, and whether it is
        // of a class or one of its subclasses.
        if (selector == "class" && arguments.empty())
            return classValue(className(receiver));
        // Whether the receiver can respond to the
        // message a Symbol names.
        if ((selector == "recognizes:" || selector == "respondsTo:") && arguments.size() == 1)
        {
            if (arguments[0].kind != Value::Kind::Symbol && arguments[0].kind != Value::Kind::String)
                throw Failure{selector + " takes the Symbol of a message", range};
            return Value::of(recognizes(receiver, arguments[0].text, range));
        }
        if (selector == "isKindOf:" && arguments.size() == 1)
        {
            const auto wanted = classNameOf(arguments[0]);
            if (!wanted.has_value())
                throw Failure{"isKindOf: takes a class", range};
            for (auto name = std::string{className(receiver)};;)
            {
                if (name == *wanted)
                    return Value::of(true);
                const auto parent = superclasses().find(name);
                if (parent == superclasses().end())
                    return Value::of(false);
                name = parent->second;
            }
        }
        if (selector == "perform:withArguments:" && arguments.size() == 2)
        {
            if (arguments[1].kind != Value::Kind::Array)
                throw Failure{"perform:withArguments: takes an Array of arguments", range};
            std::vector<Value> all{arguments[0]};
            all.insert(all.end(), arguments[1].elements->begin(), arguments[1].elements->end());
            const auto keywords = static_cast<std::size_t>(std::ranges::count(arguments[0].text, ':'));
            const auto binary = !arguments[0].text.empty()
                && std::isalpha(static_cast<unsigned char>(arguments[0].text.front())) == 0;
            if ((arguments[0].kind != Value::Kind::Symbol && arguments[0].kind != Value::Kind::String)
                || (binary ? 1U : keywords) != all.size() - 1)
                throw Failure{"perform:withArguments: takes a selector and as many arguments as it has", range};
            return send(receiver, arguments[0].text, std::span<const Value>{all}.subspan(1), range);
        }
        // perform: sends the receiver the message a Symbol names, its
        // arguments after it: 4 perform: #c is 4 c.
        if ((selector == "perform:" || selector == "perform:with:" || selector == "perform:with:with:")
            && !arguments.empty())
        {
            if (arguments[0].kind != Value::Kind::Symbol && arguments[0].kind != Value::Kind::String)
                throw Failure{"perform: takes the Symbol of a message", range};
            const auto named = arguments[0].text;
            // A binary message, as #+, takes one argument, a keyword
            // message one for each colon.
            const auto binary = !named.empty() && std::isalpha(static_cast<unsigned char>(named.front())) == 0;
            const auto taken = binary ? std::size_t{1} : static_cast<std::size_t>(std::ranges::count(named, ':'));
            if (taken != arguments.size() - 1)
                throw Failure{"#" + named + " takes another number of arguments", range};
            return send(receiver, named, arguments.subspan(1), range);
        }
        // A file type, or a list of them, as a filter of a directory's
        // contents.
        if (selector == "asFileTypeFilter" && arguments.empty())
        {
            const auto types = receiver.kind == Value::Kind::Array ? *receiver.elements : std::vector<Value>{receiver};
            for (const auto& type : types)
                (void)extensionsOfType(type, range);
            return tagged(fileTypeFilterTag, types);
        }
        // An Association of an object with another, as ?sample1 ->
        // 'siren.aif'.
        if (selector == "->" && arguments.size() == 1)
            return tagged(associationTag, {receiver, arguments[0]});
        // A value drawn on a Tool's View, as a Rectangle by its border:
        // the host draws it.
        if ((selector == "displayStrokedOn:" || selector == "displayFilledOn:") && arguments.size() == 1)
        {
            if (trial_)
                throw TrialStop{true};
            std::string error;
            if (auto answer = host_.send(receiver, selector, arguments, error))
                return *answer;
            throw Failure{error.empty() ? "The value does not understand " + selector : error, range};
        }
        // What any object prints as, and printing it on a stream.
        if ((selector == "printString" || selector == "displayString") && arguments.empty())
            return Value::string(printed(receiver, selector == "displayString", 0));
        // printOn: abbreviates a long collection, printAllOn: prints every
        // element.
        if ((selector == "printOn:" || selector == "printAllOn:") && arguments.size() == 1)
        {
            abbreviates_ = selector == "printOn:";
            const std::array text{Value::string(printed(receiver, false, 0))};
            abbreviates_ = true;
            send(arguments[0], "nextPutAll:", text, range);
            return receiver;
        }
        if (selector == "notNil")
            return Value::of(receiver.kind != Value::Kind::Nil);
        if (selector == "ifNil:")
            return receiver.kind == Value::Kind::Nil ? callBlock(arguments[0], {}, range) : receiver;
        if (selector == "ifNotNil:")
        {
            if (receiver.kind == Value::Kind::Nil)
                return receiver;
            return callBlock(arguments[0], blockArguments(arguments[0], receiver), range);
        }
        if (selector == "ifNil:ifNotNil:")
        {
            return receiver.kind == Value::Kind::Nil
                ? callBlock(arguments[0], {}, range)
                : callBlock(arguments[1], blockArguments(arguments[1], receiver), range);
        }
        if (selector == "=")
            return Value::of(equal(receiver, arguments[0], range));
        if (selector == "~=")
            return Value::of(!equal(receiver, arguments[0], range));
        if (selector == "==")
            return Value::of(identical(receiver, arguments[0], range));
        if (selector == "~~")
            return Value::of(!identical(receiver, arguments[0], range));
        // What kind of object the receiver is; a Symbol
        // is a String too, and strings, Arrays and collections are
        // collections.
        if (selector == "isNumber")
            return Value::of(receiver.kind == Value::Kind::Number);
        if (selector == "isInteger")
        {
            return Value::of(receiver.kind == Value::Kind::Number && std::isfinite(receiver.number)
                && receiver.number == std::floor(receiver.number));
        }
        if (selector == "isString")
            return Value::of(receiver.kind == Value::Kind::String || receiver.kind == Value::Kind::Symbol);
        if (selector == "isSymbol")
            return Value::of(receiver.kind == Value::Kind::Symbol);
        if (selector == "isArray")
            return Value::of(receiver.kind == Value::Kind::Array);
        if (selector == "isBoolean")
            return Value::of(receiver.kind == Value::Kind::Boolean);
        if (selector == "isCollection")
        {
            return Value::of(receiver.kind == Value::Kind::Array || receiver.kind == Value::Kind::String
                || receiver.kind == Value::Kind::Symbol
                || (receiver.kind == Value::Kind::Object && receiver.index >= firstCollection
                    && receiver.index - firstCollection < collections_.size())
                || (receiver.kind == Value::Kind::Object && receiver.index >= firstDictionary
                    && receiver.index - firstDictionary < dictionaries_.size()));
        }
        switch (receiver.kind)
        {
        case Value::Kind::Object:
        {
            // Float pi, Float e and Float phi, the golden ratio.
            if (receiver.index == floatClass && arguments.empty() && selector == "errorTolerance")
                return sendClassValue("Float", selector, arguments, range);
            if (receiver.index == floatClass && arguments.empty()
                && (selector == "pi" || selector == "e" || selector == "phi"))
            {
                return Value::of(selector == "pi" ? std::numbers::pi
                        : selector == "e"         ? std::numbers::e
                                                  : std::numbers::phi);
            }
            if (receiver.index == dateClass || receiver.index == timeClass || dateOrTime(receiver))
                return sendDateOrTime(receiver, selector, arguments, range);
            // A graph of points joined by lines, or
            // of values at x 1, 2, 3 and on, for the host to open.
            if (receiver.index == filenameClass)
                return sendFilenameClass(selector, arguments, range);
            if (receiver.index == xyGraphViewClass)
            {
                if (selector != "openOnPoints:" && selector != "openOnValues:")
                    throw Failure{"XYGraphView understands openOnPoints: and openOnValues:", range};
                XYGraph graph;
                for (const auto& element : elementsOf(arguments[0], range))
                {
                    if (graph.size() >= maximumGraphPoints)
                        throw Failure{"A graph holds at most 4096 points", range};
                    if (selector == "openOnPoints:")
                    {
                        const auto point = pointOf(element);
                        if (!point.has_value())
                            throw Failure{"openOnPoints: takes a collection of Points", range};
                        graph.push_back(*point);
                    }
                    else
                    {
                        if (element.kind != Value::Kind::Number)
                            throw Failure{"openOnValues: takes a collection of numbers", range};
                        graph.emplace_back(static_cast<double>(graph.size() + 1), element.number);
                    }
                }
                if (!trial_ && graphs_ != nullptr && graphs_->size() < maximumGraphs)
                    graphs_->push_back(std::move(graph));
                return Value::nil();
            }
            // Point x: 100 y: 100, as 100 @ 100.
            if (receiver.index == pointClass)
            {
                if (selector != "x:y:" || arguments[0].kind != Value::Kind::Number
                    || arguments[1].kind != Value::Kind::Number || arguments[0].unit != Unit::None
                    || arguments[1].unit != Unit::None)
                    throw Failure{"Point understands x:y:, of numbers without units", range};
                return newPoint({arguments[0].number, arguments[1].number}, range);
            }
            if (const auto* parts = partsOf(receiver, classTag))
                return sendClassValue((*parts)[0].text, selector, arguments, range);
            // An image read from a GIF file.
            if (const auto* parts = partsOf(receiver, imageTag))
            {
                if (selector == "width" && arguments.empty())
                    return (*parts)[0];
                if (selector == "height" && arguments.empty())
                    return (*parts)[1];
                if (selector == "extent" && arguments.empty())
                    return newPoint({(*parts)[0].number, (*parts)[1].number}, range);
                throw Failure{"An Image understands width, height and extent", range};
            }
            if (const auto* parts = partsOf(receiver, associationTag))
            {
                if (selector == "key" && arguments.empty())
                    return (*parts)[0];
                if (selector == "value" && arguments.empty())
                    return (*parts)[1];
                throw Failure{"An Association understands key and value", range};
            }
            // BindingList new, and BindingList with: an Association with:
            // another, an Array of them.
            if (receiver.index == bindingListClass)
            {
                const auto withs = selector.size() == 5 * arguments.size()
                    && std::ranges::all_of(std::views::iota(std::size_t{0}, arguments.size()),
                        [&](std::size_t at) { return selector.compare(5 * at, 5, "with:") == 0; });
                if (selector == "new" || (!arguments.empty() && withs))
                {
                    for (const auto& argument : arguments)
                    {
                        if (partsOf(argument, associationTag) == nullptr)
                            throw Failure{"A BindingList holds Associations, as ?name -> value", range};
                    }
                    return Value::array({arguments.begin(), arguments.end()});
                }
                throw Failure{"BindingList understands new and with:", range};
            }
            if (complexOf(receiver).has_value())
                return sendComplex(*complexOf(receiver), selector, arguments, range);
            if (pointOf(receiver).has_value())
                return sendPoint(*pointOf(receiver), selector, arguments, range);
            if (const auto rectangle = rectangleOf(receiver))
                return sendRectangle(*rectangle, selector, arguments, range);
            if (receiver.index == hostDriverInterfaceClass)
                return sendHostDriverInterface(selector, arguments, range);
            if (receiver.index == preferencesClass)
                return sendPreferences(selector, arguments, range);
            if (receiver.index == rectangleClass)
                return sendRectangleClass(selector, arguments, range);
            if (filenameOf(receiver).has_value())
                return sendFilename(*filenameOf(receiver), selector, arguments, range);
            if (receiver.index == samplesFileClass
                || (receiver.index >= firstSamplesFile && receiver.index - firstSamplesFile < samplesFiles_.size()))
                return sendSamplesFile(receiver, selector, arguments, range);
            if (receiver.index == randomClass
                || (receiver.index >= firstRandom && receiver.index - firstRandom < randoms_.size()))
                return sendRandom(receiver, selector, arguments, range);
            if (receiver.index == oneOverFClass
                || (receiver.index >= firstOneOverF && receiver.index - firstOneOverF < oneOverFs_.size()))
                return sendOneOverF(receiver, selector, arguments, range);
            if (receiver.index == markovChainClass
                || (receiver.index >= firstMarkovChain && receiver.index - firstMarkovChain < markovChains_.size()))
                return sendMarkovChain(receiver, selector, arguments, range);
            if (receiver.index == arrayClass)
                return sendArrayClass(selector, arguments, range);
            if (receiver.index == readStreamClass || receiver.index == writeStreamClass
                || receiver.index == readWriteStreamClass
                || (receiver.index >= firstReadStream && receiver.index - firstReadStream < streams_.size()))
                return sendReadStream(receiver, selector, arguments, range);
            if (receiver.index == orderedCollectionClass || receiver.index == setClass || receiver.index == bagClass
                || receiver.index == sortedCollectionClass
                || (receiver.index >= firstCollection && receiver.index - firstCollection < collections_.size()))
                return sendCollection(receiver, selector, arguments, range);
            if (receiver.index == dictionaryClass
                || (receiver.index >= firstDictionary && receiver.index - firstDictionary < dictionaries_.size()))
                return sendDictionary(receiver, selector, arguments, range);
            // String new, empty, and String new: n, of n spaces.
            if (receiver.index == stringClass)
            {
                if (selector == "new")
                    return Value::string(std::string{});
                if (selector == "new:" && arguments[0].kind == Value::Kind::Number && arguments[0].number >= 0.0
                    && arguments[0].number <= static_cast<double>(maximumElements))
                    return Value::string(std::string(static_cast<std::size_t>(arguments[0].number), ' '));
                throw Failure{"String understands new and new:", range};
            }
            if (trial_ && receiver.index == scriptSelf)
                throw TrialStop{false};
            if (receiver.index == scriptSelf)
                return sendSelf(selector, arguments, range);
            if (trial_)
                throw TrialStop{false};
            std::string error;
            if (auto answer = host_.send(receiver, selector, arguments, error))
                return *answer;
            throw Failure{error.empty() ? "The object does not understand " + selector : error, range};
        }
        case Value::Kind::Boolean:
            return sendBoolean(receiver.boolean, selector, arguments, range);
        case Value::Kind::Block:
            return sendBlock(receiver, selector, arguments, range);
        case Value::Kind::Number:
            return sendNumber(receiver, selector, arguments, range);
        case Value::Kind::Expression:
            return sendExpression(receiver, selector, arguments, range);
        case Value::Kind::Array:
            return sendArray(receiver, selector, arguments, range);
        case Value::Kind::String:
        case Value::Kind::Symbol:
            if (selector == "," && !arguments.empty()
                && (arguments[0].kind == Value::Kind::String || arguments[0].kind == Value::Kind::Symbol))
            {
                return Value::string(receiver.text + arguments[0].text);
            }
            if (selector == "size")
                return Value::of(static_cast<double>(receiver.text.size()));
            if (selector == "isEmpty" || selector == "notEmpty")
                return Value::of(receiver.text.empty() == (selector == "isEmpty"));
            if (selector == "asString")
                return Value::string(receiver.text);
            // A string's characters by position from 1, a part of it, and
            // the symbol of its name.
            if (selector == "at:" || selector == "copyFrom:to:")
            {
                const auto position = [&](const Value& index) {
                    if (index.kind != Value::Kind::Number || index.number != std::floor(index.number)
                        || index.number < 1.0 || index.number > static_cast<double>(receiver.text.size()))
                    {
                        throw Failure{"The index is outside the string", range};
                    }
                    return static_cast<std::size_t>(index.number) - 1;
                };
                if (selector == "at:")
                    return Value::string(std::string(1, receiver.text[position(arguments[0])]));
                const auto from = position(arguments[0]);
                const auto to = position(arguments[1]);
                return Value::string(to < from ? std::string{} : receiver.text.substr(from, to - from + 1));
            }
            if (selector == "asSymbol")
            {
                auto symbol = Value::string(receiver.text);
                symbol.kind = Value::Kind::Symbol;
                return symbol;
            }
            return sendString(receiver, selector, arguments, range);
        default:
            break;
        }
        throw Failure{"The value does not understand " + selector, range};
    }

    // The messages to Strings: comparing in ASCII with
    // case ignored, finding, matching, converting and joining; the
    // messages to collections act on its characters, and a copy of them is
    // a string again.
    Value sendString(const Value& receiver, const std::string& selector, std::span<const Value> arguments,
        const SourceRange& range)
    {
        const auto& text = receiver.text;
        // A String names a file, or an Event
        // Value.
        if (arguments.empty() && selector == "asFilename")
            return newFilename(text, range);
        if (arguments.empty() && selector == "asSampleFile")
        {
            auto found = host_.sampleFile(text);
            if (!found.has_value())
                throw Failure{"No sample file is named " + text, range};
            return newSamplesFile(std::move(*found), range);
        }
        if (arguments.empty() && selector == "fileExtension")
            return extensionOf(text);
        if (arguments.empty() && selector == "asHotValue")
        {
            if (text.empty() || std::isalpha(static_cast<unsigned char>(text.front())) == 0
                || !std::ranges::all_of(text, [](char character) {
                       return std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_';
                   }))
                throw Failure{"An Event Value's name is a letter followed by letters, digits and underscores", range};
            return Value::expression("!" + text);
        }
        const auto textOf = [&](const Value& value) {
            if (value.kind != Value::Kind::String && value.kind != Value::Kind::Symbol)
                throw Failure{selector + " takes a string", range};
            return value.text;
        };
        const auto folded = [](std::string value) {
            for (auto& character : value)
                character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
            return value;
        };
        if (selector == "<" || selector == "<=" || selector == ">" || selector == ">=")
        {
            const auto order = folded(text).compare(folded(textOf(arguments[0])));
            return Value::of(selector == "<" ? order < 0 : selector == "<=" ? order <= 0
                    : selector == ">" ? order > 0 : order >= 0);
        }
        if (selector == "trueCompare:")
        {
            const auto order = text.compare(textOf(arguments[0]));
            return Value::of(order < 0 ? -1.0 : order > 0 ? 1.0 : 0.0);
        }
        if (selector == "beginsWith:")
            return Value::of(text.starts_with(textOf(arguments[0])));
        if (selector == "endsWith:")
            return Value::of(text.ends_with(textOf(arguments[0])));
        if (selector == "findString:startingAt:")
        {
            const auto start = arguments[1];
            if (start.kind != Value::Kind::Number || start.number < 1.0 || start.number != std::floor(start.number))
                throw Failure{"findString:startingAt: starts at a position from 1", range};
            const auto found = text.find(textOf(arguments[0]), static_cast<std::size_t>(start.number) - 1);
            return Value::of(found == std::string::npos ? 0.0 : static_cast<double>(found + 1));
        }
        if (selector == "match:ignoreCase:")
        {
            // The receiver is the pattern: # matches any one character and *
            // any run of them.
            const auto ignore = truth(arguments[1], range);
            const auto pattern = ignore ? folded(text) : text;
            const auto candidate = ignore ? folded(textOf(arguments[0])) : textOf(arguments[0]);
            std::vector<std::vector<char>> matches(pattern.size() + 1, std::vector<char>(candidate.size() + 1, 0));
            matches[0][0] = 1;
            for (std::size_t p = 1; p <= pattern.size(); ++p)
            {
                if (pattern[p - 1] == '*')
                    matches[p][0] = matches[p - 1][0];
                for (std::size_t c = 1; c <= candidate.size(); ++c)
                {
                    if (pattern[p - 1] == '*')
                        matches[p][c] = matches[p - 1][c] || matches[p][c - 1];
                    else if (pattern[p - 1] == '#' || pattern[p - 1] == candidate[c - 1])
                        matches[p][c] = matches[p - 1][c - 1];
                }
            }
            return Value::of(matches[pattern.size()][candidate.size()] != 0);
        }
        if (selector == "&")
        {
            const auto& other = arguments[0];
            const auto printed = other.kind == Value::Kind::String || other.kind == Value::Kind::Symbol
                ? std::optional<std::string>{other.text} : fieldText(other);
            if (!printed.has_value())
                throw Failure{"& joins a printable value to a string", range};
            return Value::string(text + *printed);
        }
        if (selector == "asLowercase" || selector == "asUppercase")
        {
            auto converted = text;
            for (auto& character : converted)
            {
                const auto byte = static_cast<unsigned char>(character);
                character = static_cast<char>(selector == "asLowercase" ? std::tolower(byte) : std::toupper(byte));
            }
            return Value::string(std::move(converted));
        }
        if (selector == "copyUpTo:")
        {
            const auto stop = textOf(arguments[0]);
            const auto found = stop.size() == 1 ? text.find(stop.front()) : std::string::npos;
            return Value::string(text.substr(0, found));
        }
        if (selector == "withCRs")
        {
            auto converted = text;
            std::ranges::replace(converted, '\\', '\r');
            return Value::string(std::move(converted));
        }
        // A character is a string of one ($T): its code, as f next asInteger
        // reads a text's characters as pitches.
        if (arguments.empty() && text.size() == 1
            && (selector == "asInteger" || selector == "value" || selector == "asciiValue"))
            return Value::of(static_cast<double>(static_cast<unsigned char>(text.front())));
        if (selector == "asNumber")
        {
            // A number written out, nil for anything else.
            const auto first = text.find_first_not_of(" \t\r\n");
            const auto last = text.find_last_not_of(" \t\r\n");
            if (first == std::string::npos)
                return Value::nil();
            const auto written = text.substr(first, last - first + 1);
            char* end = nullptr;
            const auto number = std::strtod(written.c_str(), &end);
            if (end != written.c_str() + written.size() || !std::isfinite(number))
                return Value::nil();
            return Value::of(number);
        }
        // The rest as a collection of its characters; a stream on one reads
        // and writes strings.
        std::vector<Value> characters;
        for (const auto character : text)
            characters.push_back(Value::string(std::string(1, character)));
        if (selector == "readStream" || selector == "writeStream")
            return makeStream(std::move(characters), true, selector == "writeStream", range);
        std::vector<Value> converted(arguments.begin(), arguments.end());
        for (auto& argument : converted)
        {
            if ((argument.kind == Value::Kind::String || argument.kind == Value::Kind::Symbol)
                && (selector == "copyReplaceAll:with:" || selector == "copyReplaceFrom:to:with:"
                    || selector == "indexOfSubCollection:startingAt:" || selector == ","))
            {
                std::vector<Value> parts;
                for (const auto character : argument.text)
                    parts.push_back(Value::string(std::string(1, character)));
                argument = Value::array(std::move(parts));
            }
        }
        auto answer = sendArray(Value::array(characters), selector, converted, range);
        const auto copies = selector == "reverse" || selector == "reversed" || selector == "select:" || selector == "reject:"
            || selector == "copyWith:" || selector == "copyWithout:" || selector == "copyReplaceAll:with:"
            || selector == "copyReplaceFrom:to:with:";
        if (copies && answer.kind == Value::Kind::Array
            && std::ranges::all_of(*answer.elements, [](const Value& element) {
                   return element.kind == Value::Kind::String && element.text.size() == 1;
               }))
        {
            std::string joined;
            for (const auto& element : *answer.elements)
                joined += element.text;
            return Value::string(std::move(joined));
        }
        return answer;
    }

    // The receiver passed to a one-argument block, or nothing to a block
    // without arguments.
    [[nodiscard]] std::vector<Value> blockArguments(const Value& block, const Value& receiver) const
    {
        if (block.kind == Value::Kind::Block
            && closures_[block.index].program->bodies[closures_[block.index].body].parameters.size() == 1)
            return {receiver};
        return {};
    }

    // ==: two strings of the same
    // characters are not necessarily the same object, nor two Arrays of the
    // same elements; a string of one character stands for a Character,
    // unique as Characters are; other values are identical when equal.
    [[nodiscard]] bool identical(const Value& left, const Value& right, const SourceRange& range) const
    {
        if (left.kind == Value::Kind::String && right.kind == Value::Kind::String && left.identity != nullptr
            && right.identity != nullptr && (left.text.size() != 1 || right.text.size() != 1))
            return left.identity == right.identity;
        if (left.kind == Value::Kind::Array && right.kind == Value::Kind::Array && left.elements != nullptr
            && right.elements != nullptr)
            return left.elements == right.elements;
        return equal(left, right, range);
    }

    [[nodiscard]] bool equal(const Value& left, const Value& right, const SourceRange& range) const
    {
        // A Filename is = to the String of its name.
        if (const auto path = filenameOf(left); path.has_value() && right.kind == Value::Kind::String)
            return *path == right.text;
        if (left.kind != right.kind)
            return false;
        switch (left.kind)
        {
        case Value::Kind::Nil:
            return true;
        case Value::Kind::Boolean:
            return left.boolean == right.boolean;
        case Value::Kind::Number:
        {
            if (const auto* exact = exactOf(left); exact != nullptr && exactOf(right) != nullptr)
                return ExactNumber::compare(*exact, *exactOf(right)) == 0;
            const auto unitsMatch = quantity(left.unit) == quantity(right.unit)
                && (quantity(left.unit) != Quantity::Other || left.unit == right.unit);
            return unitsMatch && toBase(left.number, left.unit) == toBase(right.number, right.unit);
        }
        case Value::Kind::String:
        case Value::Kind::Symbol:
        case Value::Kind::Expression:
            return left.text == right.text;
        case Value::Kind::Object:
            if (complexOf(left).has_value() && complexOf(right).has_value())
                return *complexOf(left) == *complexOf(right);
            if (pointOf(left).has_value() && pointOf(right).has_value())
                return *pointOf(left) == *pointOf(right);
            if (rectangleOf(left).has_value() && rectangleOf(right).has_value())
                return *rectangleOf(left) == *rectangleOf(right);
            if (filenameOf(left).has_value() && filenameOf(right).has_value())
                return *filenameOf(left) == *filenameOf(right);
            // Dates are the same day, Times the same second.
            if (dateOrTime(left) && dateOrTime(right))
                return isTime(left) == isTime(right) && moment(left) == moment(right);
            if (partsOf(left, associationTag) != nullptr && partsOf(right, associationTag) != nullptr)
            {
                return equal((*left.elements)[0], (*right.elements)[0], range)
                    && equal((*left.elements)[1], (*right.elements)[1], range);
            }
            // A host's value of parts, as a Tool's ColorValue, is alike by
            // them.
            if (left.index == right.index && left.elements != nullptr && right.elements != nullptr)
            {
                return left.elements->size() == right.elements->size()
                    && std::ranges::equal(*left.elements, *right.elements, [&](const Value& one, const Value& other) {
                           return equal(one, other, range);
                       });
            }
            return left.index == right.index;
        case Value::Kind::Block:
            return left.index == right.index;
        case Value::Kind::Array:
        {
            if (left.elements->size() != right.elements->size())
                return false;
            for (std::size_t index = 0; index < left.elements->size(); ++index)
            {
                if (!equal((*left.elements)[index], (*right.elements)[index], range))
                    return false;
            }
            return true;
        }
        }
        throw Failure{"The values cannot be compared", range};
    }

    Value sendBoolean(bool value, const std::string& selector, std::span<const Value> arguments,
        const SourceRange& range)
    {
        // varIfTrue: and its kin choose by a comparison of ?variables,
        // whose declared values the script knows.
        if (selector == "ifTrue:" || selector == "varIfTrue:")
            return value ? callBlock(arguments[0], {}, range) : Value::nil();
        if (selector == "ifFalse:" || selector == "varIfFalse:")
            return value ? Value::nil() : callBlock(arguments[0], {}, range);
        if (selector == "ifTrue:ifFalse:" || selector == "varIfTrue:ifFalse:")
            return callBlock(arguments[value ? 0 : 1], {}, range);
        if (selector == "ifFalse:ifTrue:" || selector == "varIfFalse:ifTrue:")
            return callBlock(arguments[value ? 1 : 0], {}, range);
        if (selector == "not")
            return Value::of(!value);
        if (selector == "&")
            return Value::of(value && truth(arguments[0], range));
        if (selector == "|")
            return Value::of(value || truth(arguments[0], range));
        // varAnd: and varOr: likewise for statements with ?variables.
        if (selector == "and:" || selector == "varAnd:")
            return value ? Value::of(truth(callBlock(arguments[0], {}, range), range)) : Value::of(false);
        if (selector == "or:" || selector == "varOr:")
            return value ? Value::of(true) : Value::of(truth(callBlock(arguments[0], {}, range), range));
        // Both alike, or not.
        if (selector == "eqv:" || selector == "xor:")
            return Value::of((value == truth(arguments[0], range)) == (selector == "eqv:"));
        throw Failure{"A boolean does not understand " + selector, range};
    }

    Value sendBlock(const Value& block, const std::string& selector, std::span<const Value> arguments,
        const SourceRange& range)
    {
        if (selector == "value" || selector == "value:" || selector == "value:value:"
            || selector == "value:value:value:" || selector == "value:value:value:value:")
        {
            return callBlock(block, arguments, range);
        }
        if (selector == "numArgs")
        {
            const auto& closure = closures_[block.index];
            return Value::of(static_cast<double>(closure.program->bodies[closure.body].parameters.size()));
        }
        if (selector == "whileTrue:" || selector == "whileFalse:")
        {
            const auto until = selector == "whileFalse:";
            while (truth(callBlock(block, {}, range), range) != until)
                callBlock(arguments[0], {}, range);
            return Value::nil();
        }
        if (selector == "whileTrue" || selector == "whileFalse")
        {
            const auto until = selector == "whileFalse";
            while (truth(callBlock(block, {}, range), range) != until)
            {
            }
            return Value::nil();
        }
        throw Failure{"A block does not understand " + selector, range};
    }

    [[nodiscard]] Value arithmetic(const Value& left, const std::string& selector, const Value& right,
        const SourceRange& range) const
    {
        const auto leftQuantity = quantity(left.unit);
        const auto rightQuantity = quantity(right.unit);
        if (selector == "+" || selector == "-")
        {
            const auto sign = selector == "+" ? 1.0 : -1.0;
            if (left.unit == right.unit || rightQuantity == Quantity::None)
                return Value::of(left.number + sign * right.number, left.unit);
            if (leftQuantity == Quantity::None)
                return Value::of(left.number + sign * right.number, right.unit);
            if (leftQuantity == rightQuantity && leftQuantity != Quantity::Other)
            {
                // Mixed frequencies add in hertz, mixed times in seconds.
                return Value::of(toBase(left.number, left.unit) + sign * toBase(right.number, right.unit),
                    baseUnit(leftQuantity));
            }
            throw Failure{"Values of these units cannot be added", range};
        }
        if (selector == "*")
        {
            if (rightQuantity == Quantity::None)
                return Value::of(left.number * right.number, left.unit);
            if (leftQuantity == Quantity::None)
                return Value::of(left.number * right.number, right.unit);
            throw Failure{"Values with units cannot be multiplied together", range};
        }
        if (selector == "/" || selector == "//" || selector == "\\\\")
        {
            double numerator = left.number;
            double denominator = right.number;
            auto unit = left.unit;
            if (rightQuantity != Quantity::None)
            {
                if (leftQuantity == rightQuantity && (leftQuantity != Quantity::Other || left.unit == right.unit))
                {
                    numerator = toBase(left.number, left.unit);
                    denominator = toBase(right.number, right.unit);
                    unit = Unit::None;
                }
                else if (leftQuantity == Quantity::None && selector == "/"
                    && (rightQuantity == Quantity::Frequency || rightQuantity == Quantity::Time))
                {
                    // A number over a time is a frequency, and over a
                    // frequency a time.
                    denominator = toBase(right.number, right.unit);
                    unit = rightQuantity == Quantity::Time ? Unit::Hertz : Unit::Seconds;
                }
                else
                {
                    throw Failure{"Values of these units cannot be divided", range};
                }
            }
            if (denominator == 0.0)
                throw Failure{"Division by zero", range};
            if (selector == "/")
                return Value::of(numerator / denominator, unit);
            const auto quotient = std::floor(numerator / denominator);
            if (selector == "//")
                return Value::of(quotient, Unit::None);
            return Value::of(numerator - quotient * denominator, unit);
        }
        throw Failure{"A number does not understand " + selector, range};
    }

    [[nodiscard]] double comparable(const Value& value) const noexcept
    {
        return toBase(value.number, value.unit);
    }

    [[nodiscard]] static Value exactValue(ExactNumber value, const SourceRange& range)
    {
        if (value.bitLength() > LargeInteger::maximumBits)
            throw Failure{"The number is larger than a script holds", range};
        return Value::of(std::move(value));
    }

    // Integers and ratios of them stay exact, a
    // ratio in its most reduced form, through arithmetic, comparisons and
    // the integer messages; nothing for a message an exact number answers
    // as floating point, or with a floating point argument.
    std::optional<Value> sendExact(const Value& number, const ExactNumber& exact, const std::string& selector,
        std::span<const Value> arguments, const SourceRange& range)
    {
        const ExactNumber one = ExactNumber::integer(LargeInteger{1});
        if (arguments.empty())
        {
            if (selector == "negated")
                return exactValue(exact.negated(), range);
            if (selector == "abs")
                return exactValue(exact.abs(), range);
            if (selector == "squared")
                return exactValue(ExactNumber::multiply(exact, exact), range);
            // Format 156: in a project taking the table rounding
            // (RoundingRule::ManualTable), rounded adds 1/2 and truncates
            // toward negative infinity and truncated rounds toward negative
            // infinity.
            const auto manual = host_.rounding() == talas::expr::RoundingRule::ManualTable;
            if (selector == "rounded" && manual)
            {
                const auto half = ExactNumber::ratio(LargeInteger{1}, LargeInteger{2});
                return exactValue(ExactNumber::integer(ExactNumber::add(exact, *half).floor()), range);
            }
            if (selector == "truncated" && manual)
                return exactValue(ExactNumber::integer(exact.floor()), range);
            if (selector == "rounded")
                return exactValue(ExactNumber::integer(exact.rounded()), range);
            if (selector == "truncated" || selector == "asInteger")
                return exactValue(ExactNumber::integer(exact.truncated()), range);
            if (selector == "floor")
                return exactValue(ExactNumber::integer(exact.floor()), range);
            if (selector == "ceiling")
                return exactValue(ExactNumber::integer(exact.ceiling()), range);
            if (selector == "reciprocal" || selector == "inverse")
            {
                auto inverse = ExactNumber::divide(one, exact);
                if (!inverse.has_value())
                    throw Failure{"Division by zero", range};
                return exactValue(std::move(*inverse), range);
            }
            if (selector == "numerator")
                return exactValue(ExactNumber::integer(exact.numerator), range);
            if (selector == "denominator")
                return exactValue(ExactNumber::integer(exact.denominator), range);
            if (selector == "isFraction")
                return Value::of(!exact.isInteger());
            if (selector == "printString" || selector == "displayString" || selector == "asString")
                return Value::string(exact.toString());
            if ((selector == "even" || selector == "odd") && exact.isInteger())
                return Value::of(exact.numerator.isOdd() == (selector == "odd"));
            if (selector == "factorial")
            {
                const auto count = exact.isInteger() ? exact.numerator.toInt64() : std::nullopt;
                if (!count.has_value() || *count < 0)
                    throw Failure{"factorial takes an integer of 0 or more", range};
                if (*count > 10'000)
                    throw Failure{"factorial takes an integer of at most 10000 here", range};
                auto product = LargeInteger{1};
                for (std::int64_t factor = 2; factor <= *count; ++factor)
                {
                    product = product * LargeInteger{factor};
                    if (product.bitLength() > LargeInteger::maximumBits)
                        throw Failure{"The number is larger than a script holds", range};
                }
                return exactValue(ExactNumber::integer(std::move(product)), range);
            }
            return std::nullopt;
        }
        if (arguments.size() == 1 && selector == "printStringRadix:" && exact.isInteger())
        {
            const auto* base = exactOf(arguments[0]);
            const auto radix = base != nullptr && base->isInteger() ? base->numerator.toInt64() : std::nullopt;
            if (!radix.has_value() || *radix < 2 || *radix > 36)
                throw Failure{"printStringRadix: takes an integer and a base from 2 to 36", range};
            return Value::string(exact.numerator.toString(static_cast<unsigned>(*radix)));
        }
        // An Interval of exact numbers, and loops over one.
        if ((selector == "to:" || selector == "to:by:" || selector == "to:do:" || selector == "to:by:do:")
            && arguments.size() >= 1)
        {
            const auto stepped = selector == "to:by:" || selector == "to:by:do:";
            const auto* last = exactOf(arguments[0]);
            const auto* step = stepped ? exactOf(arguments[1]) : &one;
            if (last == nullptr || step == nullptr)
                return std::nullopt;
            if (step->numerator.isZero())
                throw Failure{"An Interval needs a nonzero step", range};
            const auto rising = !step->numerator.isNegative();
            const auto looping = selector.ends_with("do:");
            std::vector<Value> values;
            for (auto index = exact;
                 rising ? ExactNumber::compare(index, *last) <= 0 : ExactNumber::compare(index, *last) >= 0;
                 index = ExactNumber::add(index, *step))
            {
                if (looping)
                {
                    const std::array argument{Value::of(index)};
                    callBlock(arguments[stepped ? 2 : 1], argument, range);
                    continue;
                }
                if (values.size() >= maximumElements)
                    throw Failure{"An Interval exceeds the fixed element limit", range};
                values.push_back(Value::of(index));
            }
            return looping ? number : Value::array(std::move(values));
        }
        if (arguments.size() == 2 && selector == "between:and:")
        {
            const auto* low = exactOf(arguments[0]);
            const auto* high = exactOf(arguments[1]);
            if (low == nullptr || high == nullptr)
                return std::nullopt;
            return Value::of(ExactNumber::compare(exact, *low) >= 0 && ExactNumber::compare(exact, *high) <= 0);
        }
        if (arguments.size() != 1)
            return std::nullopt;
        const auto* other = exactOf(arguments[0]);
        if (other == nullptr)
            return std::nullopt;
        if (selector == "+")
            return exactValue(ExactNumber::add(exact, *other), range);
        if (selector == "-")
            return exactValue(ExactNumber::subtract(exact, *other), range);
        if (selector == "*")
            return exactValue(ExactNumber::multiply(exact, *other), range);
        if (selector == "/")
        {
            auto quotient = ExactNumber::divide(exact, *other);
            if (!quotient.has_value())
                throw Failure{"Division by zero", range};
            return exactValue(std::move(*quotient), range);
        }
        // // and \\ (mod:) divide toward negative infinity, quo: and rem:
        // toward zero.
        if (selector == "//" || selector == "\\\\" || selector == "mod:" || selector == "quo:" || selector == "rem:")
        {
            const auto quotient = ExactNumber::divide(exact, *other);
            if (!quotient.has_value())
                throw Failure{"Division by zero", range};
            const auto whole = ExactNumber::integer(
                selector == "quo:" || selector == "rem:" ? quotient->truncated() : quotient->floor());
            if (selector == "//" || selector == "quo:")
                return exactValue(whole, range);
            return exactValue(ExactNumber::subtract(exact, ExactNumber::multiply(*other, whole)), range);
        }
        if ((selector == "gcd:" || selector == "lcm:") && exact.isInteger() && other->isInteger())
        {
            const auto divisor = LargeInteger::gcd(exact.numerator, other->numerator);
            if (selector == "gcd:")
                return exactValue(ExactNumber::integer(divisor), range);
            if (divisor.isZero())
                return exactValue(ExactNumber::integer(LargeInteger{}), range);
            LargeInteger quotient;
            LargeInteger remainder;
            LargeInteger::divide(exact.numerator, divisor, quotient, remainder);
            return exactValue(ExactNumber::integer((quotient * other->numerator).abs()), range);
        }
        if (selector == "<" || selector == ">" || selector == "<=" || selector == ">=")
        {
            const auto order = ExactNumber::compare(exact, *other);
            return Value::of(selector == "<" ? order < 0
                    : selector == ">"        ? order > 0
                    : selector == "<="       ? order <= 0
                                             : order >= 0);
        }
        if (selector == "min:" || selector == "max:" || selector == "vmin:" || selector == "vmax:")
        {
            const auto order = ExactNumber::compare(exact, *other);
            const auto keepSelf = selector == "min:" || selector == "vmin:" ? order <= 0 : order >= 0;
            return keepSelf ? number : arguments[0];
        }
        if ((selector == "raisedTo:" || selector == "**") && other->isInteger())
        {
            const auto power = other->numerator.toInt64();
            if (!power.has_value())
                throw Failure{"The number is larger than a script holds", range};
            const auto magnitude = static_cast<std::uint64_t>(*power < 0 ? -*power : *power);
            // 0, 1 and -1 raised to any power, and others while the result
            // stays within what a script holds.
            if (exact.bitLength() > 1 && exact.bitLength() * magnitude > LargeInteger::maximumBits)
                throw Failure{"The number is larger than a script holds", range};
            auto result = one;
            auto base = exact;
            for (auto bits = magnitude; bits != 0; bits >>= 1)
            {
                if ((bits & 1U) != 0)
                    result = ExactNumber::multiply(result, base);
                if (bits > 1)
                    base = ExactNumber::multiply(base, base);
            }
            if (*power < 0)
            {
                auto inverse = ExactNumber::divide(one, result);
                if (!inverse.has_value())
                    throw Failure{"Division by zero", range};
                result = std::move(*inverse);
            }
            return exactValue(std::move(result), range);
        }
        return std::nullopt;
    }

    Value sendNumber(const Value& number, const std::string& selector, std::span<const Value> arguments,
        const SourceRange& range)
    {
        const auto finite = [&](Value value) {
            if (value.kind == Value::Kind::Number && !std::isfinite(value.number))
                throw Failure{"The result is not finite", range};
            return value;
        };
        if (const auto* exact = exactOf(number))
        {
            if (auto answer = sendExact(number, *exact, selector, arguments, range))
                return *answer;
        }
        // X @ y is a Point, and clipTo01 keeps
        // a number between 0 and 1.
        if (number.unit == Unit::None && selector == "@" && arguments.size() == 1)
        {
            const auto y = numberOf(arguments[0]);
            if (y.kind != Value::Kind::Number || y.unit != Unit::None)
                throw Failure{"@ takes a number without units", range};
            return newPoint({number.number, y.number}, range);
        }
        if (selector == "clipTo01" && arguments.empty())
            return Value::of(std::clamp(number.number, 0.0, 1.0), number.unit);
        if (selector == "asSMPTE" && arguments.empty())
        {
            const auto seconds = toBase(number.number, number.unit);
            if (quantity(number.unit) != Quantity::Time || !(seconds >= 0.0) || seconds > 1.0e9)
                throw Failure{"asSMPTE writes a time from zero as time code", range};
            return Value::string(smpteText(seconds, host_.timecodeFramesPerSecond()));
        }
        if (number.unit == Unit::None && arguments.size() == 1 && pointOf(arguments[0]).has_value()
            && (selector == "+" || selector == "-" || selector == "*" || selector == "/"))
        {
            return pointArithmetic({number.number, number.number}, selector, *pointOf(arguments[0]), range);
        }
        // 3 % 4 is the complex number 3 + 4i, and a
        // real number one whose imaginary part is 0.
        if (number.unit == Unit::None)
        {
            if (selector == "%" && arguments.size() == 1)
            {
                const auto imaginary = numberOf(arguments[0]);
                if (imaginary.kind != Value::Kind::Number || imaginary.unit != Unit::None)
                    throw Failure{"% takes a number without units", range};
                return newComplex({number.number, imaginary.number}, range);
            }
            if (arguments.size() == 1 && complexOf(arguments[0]).has_value()
                && (selector == "+" || selector == "-" || selector == "*" || selector == "/"))
            {
                return complexArithmetic({number.number, 0.0}, selector, *complexOf(arguments[0]), range);
            }
            if (arguments.empty())
            {
                if (selector == "real" || selector == "asDouble")
                    return Value::of(number.number);
                if (selector == "imaginary")
                    return Value::of(0.0);
                if (selector == "isComplex")
                    return Value::of(false);
                if (selector == "conjugated")
                    return number;
                if (selector == "magnitudeSquared")
                    return finite(Value::of(number.number * number.number));
                if (selector == "angle")
                    return Value::of(number.number < 0.0 ? std::numbers::pi : 0.0);
                if (selector == "complexSqrt")
                {
                    return number.number >= 0.0 ? Value::of(std::sqrt(number.number))
                                                : newComplex({0.0, std::sqrt(-number.number)}, range);
                }
            }
        }
        // An octave and a pitch name, 4 a, is a note number, raised or
        // lowered a half step by sharp or flat.
        if (const auto pitch = pitchNamed(selector); pitch.has_value() && arguments.empty())
        {
            if (number.unit != Unit::None || number.number != std::floor(number.number))
                throw Failure{"A pitch name follows a whole octave number", range};
            return Value::of((number.number + 1.0) * 12.0 + *pitch, Unit::NoteNumber);
        }
        // A CSound octave point pitch class, 8.09 the a above middle c
        //: its note number.
        if (selector == "octavePointPitchClass" && arguments.empty())
        {
            if (number.unit != Unit::None)
                throw Failure{"An octave point pitch class is a plain number", range};
            const auto octave = std::floor(number.number);
            const auto pitchClass = std::round((number.number - octave) * 1.0e8) / 1.0e6;
            return Value::of((octave - 3.0) * 12.0 + pitchClass, Unit::NoteNumber);
        }
        if ((selector == "sharp" || selector == "flat") && arguments.empty())
        {
            if (number.unit != Unit::NoteNumber)
                throw Failure{"Only a pitch is made " + selector, range};
            return Value::of(number.number + (selector == "sharp" ? 1.0 : -1.0), Unit::NoteNumber);
        }
        if (const auto unit = unitNamed(selector))
        {
            const auto from = quantity(number.unit);
            const auto to = quantity(*unit);
            if (number.unit == Unit::None || number.unit == *unit)
                return Value::of(number.number, *unit);
            if (from == to && from != Quantity::Other)
                return finite(Value::of(fromBase(toBase(number.number, number.unit), *unit), *unit));
            // Samples count at the host's sample rate, and a time and a
            // frequency are each other's period:
            // 440 hz s is one cycle's seconds, 3.77952 s bpm the tempo of a
            // beat that long.
            const auto rate = host_.sampleRate();
            std::optional<double> seconds;
            if (from == Quantity::Time)
                seconds = toBase(number.number, number.unit);
            else if (number.unit == Unit::Samples && rate > 0.0)
                seconds = number.number / rate;
            else if (from == Quantity::Frequency && toBase(number.number, number.unit) != 0.0)
                seconds = 1.0 / toBase(number.number, number.unit);
            if (seconds.has_value() && to == Quantity::Time)
                return finite(Value::of(fromBase(*seconds, *unit), *unit));
            if (seconds.has_value() && *unit == Unit::Samples && rate > 0.0)
                return finite(Value::of(*seconds * rate, *unit));
            if (seconds.has_value() && to == Quantity::Frequency && *seconds != 0.0)
                return finite(Value::of(fromBase(1.0 / *seconds, *unit), *unit));
            throw Failure{"A number of this unit cannot become " + selector, range};
        }
        if (arguments.size() == 1 && (selector == "+" || selector == "-" || selector == "*" || selector == "/"
                || selector == "//" || selector == "\\\\"))
        {
            const auto& other = arguments[0];
            if (other.kind == Value::Kind::Expression)
                return combine(number, selector, other, range);
            if (other.kind != Value::Kind::Number)
                throw Failure{"Arithmetic needs a number", range};
            return finite(arithmetic(number, selector, other, range));
        }
        // A comparison with a value that changes as the Sound plays is an
        // expression too, Val's 0 or 1; any other message takes the
        // numbers the script knows.
        if (arguments.size() == 1 && arguments[0].kind == Value::Kind::Expression && !arguments[0].known
            && (selector == "<" || selector == ">" || selector == "<=" || selector == ">="))
        {
            return combine(number, selector, arguments[0], range);
        }
        // A keyword message with a value that changes as the Sound plays is
        // Val's message to the number, as 1 bpm: !BPM * 2 is.
        if (selector.find(':') != std::string::npos
            && std::ranges::any_of(arguments, [](const Value& argument) {
                   return argument.kind == Value::Kind::Expression && !argument.known;
               }))
        {
            return sendExpression(Value::expression(*fieldText(number)), selector, arguments, range);
        }
        std::vector<Value> numbers;
        for (const auto& argument : arguments)
        {
            if (argument.kind == Value::Kind::Expression && !argument.known)
                throw Failure{"An Event Value has no value while the script runs, as " + selector + " needs", range};
            numbers.push_back(numberOf(argument));
        }
        arguments = numbers;
        if (arguments.size() == 1
            && (selector == "<" || selector == ">" || selector == "<=" || selector == ">="))
        {
            const auto& other = arguments[0];
            if (other.kind != Value::Kind::Number
                || (quantity(number.unit) != quantity(other.unit)
                    && quantity(number.unit) != Quantity::None && quantity(other.unit) != Quantity::None))
            {
                throw Failure{"These values cannot be compared", range};
            }
            const auto left = comparable(number);
            const auto right = comparable(other);
            return Value::of(selector == "<" ? left < right
                    : selector == ">"        ? left > right
                    : selector == "<="       ? left <= right
                                             : left >= right);
        }
        if (selector == "negated")
            return Value::of(-number.number, number.unit);
        if (selector == "abs")
            return Value::of(std::abs(number.number), number.unit);
        if (selector == "rounded" || selector == "truncated" || selector == "floor" || selector == "ceiling"
            || selector == "asInteger")
        {
            // Format 156: the table rounding (RoundingRule::ManualTable) adds
            // 0.5 and truncates toward negative infinity for rounded, so
            // -2.5 rounded is -2.0, and rounds toward negative infinity for
            // truncated, so -2.5 truncated is -3.
            const auto manual = host_.rounding() == talas::expr::RoundingRule::ManualTable;
            const auto value = selector == "rounded" && manual ? std::floor(number.number + 0.5)
                : selector == "truncated" && manual  ? std::floor(number.number)
                : selector == "rounded"              ? std::round(number.number)
                : selector == "floor"                ? std::floor(number.number)
                : selector == "ceiling"              ? std::ceil(number.number)
                                                     : std::trunc(number.number);
            return Value::of(value, number.unit);
        }
        if (selector == "asFloat" || selector == "removeUnits")
            return Value::of(number.number, selector == "removeUnits" ? Unit::None : number.unit);
        if (selector == "inverse" || selector == "reciprocal")
        {
            // A number of samples is a time at the host's sample rate, so
            // 256 samp inverse is the frequency of a 256-sample cycle
            //.
            const auto rate = host_.sampleRate();
            const auto samples = number.unit == Unit::Samples && rate > 0.0;
            const auto amount = samples ? Quantity::Time : quantity(number.unit);
            if (amount == Quantity::Other)
                throw Failure{"A number of this unit has no inverse", range};
            const auto base = samples ? number.number / rate : toBase(number.number, number.unit);
            if (base == 0.0)
                throw Failure{"Division by zero", range};
            return finite(Value::of(1.0 / base,
                amount == Quantity::Time        ? Unit::Hertz
                    : amount == Quantity::Frequency ? Unit::Seconds
                                                    : Unit::None));
        }
        if (number.unit == Unit::None)
        {
            if (selector == "sqrt")
                return finite(Value::of(std::sqrt(number.number)));
            if (selector == "squared")
                return finite(Value::of(number.number * number.number));
            if (selector == "sin" || selector == "cos" || selector == "tan" || selector == "exp" || selector == "ln"
                || selector == "log" || selector == "arcTan")
            {
                const auto value = selector == "sin" ? std::sin(number.number)
                    : selector == "cos"              ? std::cos(number.number)
                    : selector == "tan"              ? std::tan(number.number)
                    : selector == "exp"              ? std::exp(number.number)
                    : selector == "ln"               ? std::log(number.number)
                    : selector == "log"              ? std::log10(number.number)
                                                     : std::atan(number.number);
                return finite(Value::of(value));
            }
            // Val's messages to numbers answer as they do in real time.
            if (arguments.empty()
                && (selector == "twoExp" || selector == "twoLog" || selector == "sign" || selector == "wrapTo02"
                    || selector == "normCos" || selector == "normSin" || selector == "asLogicValue" || selector == "sinc"
                    || selector == "arcSin" || selector == "arcCos"))
            {
                const auto x = number.number;
                if ((selector == "arcSin" || selector == "arcCos") && (x < -1.0 || x > 1.0))
                    throw Failure{selector + " takes a number from -1 to 1", range};
                if (selector == "twoLog" && !(x > 0.0))
                    throw Failure{"twoLog takes a number above 0", range};
                const auto value = selector == "twoExp" ? std::exp2(x)
                    : selector == "twoLog"              ? std::log2(x)
                    : selector == "sign"                ? (x > 0.0 ? 1.0 : x < 0.0 ? -1.0 : 0.0)
                    : selector == "wrapTo02"            ? x - 2.0 * std::floor(x / 2.0)
                    : selector == "normCos"             ? std::cos(std::numbers::pi * x)
                    : selector == "normSin"             ? std::sin(std::numbers::pi * x)
                    : selector == "asLogicValue"        ? (x > 0.0 ? 1.0 : 0.0)
                    : selector == "sinc"                ? (x == 0.0 ? 1.0 : std::sin(x) / x)
                    : selector == "arcSin"              ? std::asin(x)
                                                        : std::acos(x);
                return finite(Value::of(value));
            }
            if (arguments.size() == 1 && (selector == "log:" || selector == "floorLog:"))
            {
                const auto base = numberOf(arguments[0]);
                if (base.kind != Value::Kind::Number || base.unit != Unit::None || !(base.number > 0.0) || base.number == 1.0
                    || !(number.number > 0.0))
                {
                    throw Failure{selector + " takes a positive base other than 1, to a positive number", range};
                }
                const auto logarithm = std::log(number.number) / std::log(base.number);
                return finite(Value::of(selector == "floorLog:" ? std::floor(logarithm + 1.0e-12) : logarithm));
            }
            // Floats' hyperbolic and Bessel functions.
            if (selector == "cosh" || selector == "sinh" || selector == "arcCosh" || selector == "arcSinh")
            {
                const auto value = selector == "cosh" ? std::cosh(number.number)
                    : selector == "sinh"              ? std::sinh(number.number)
                    : selector == "arcCosh"           ? std::acosh(number.number)
                                                      : std::asinh(number.number);
                return finite(Value::of(value));
            }
            if (selector == "besselI0")
            {
                // The modified Bessel function: the sum of ((x / 2)^k / k!)^2.
                const auto half = number.number / 2.0;
                auto term = 1.0;
                auto sum = 1.0;
                for (int k = 1; k < 500 && term > sum * 1.0e-17; ++k)
                {
                    term *= (half / k) * (half / k);
                    sum += term;
                }
                return finite(Value::of(sum));
            }
            if (selector == "besselJ0" || selector == "besselJ1")
                return finite(Value::of(selector == "besselJ0" ? ::j0(number.number) : ::j1(number.number)));
            if (selector == "besselY0" || selector == "besselY1")
            {
                if (!(number.number > 0.0))
                    throw Failure{selector + " takes a positive receiver", range};
                return finite(Value::of(selector == "besselY0" ? ::y0(number.number) : ::y1(number.number)));
            }
            if (selector == "besselJ:" || selector == "besselY:")
            {
                const auto& order = arguments[0];
                if (order.kind != Value::Kind::Number || order.number != std::floor(order.number)
                    || std::abs(order.number) > 1000.0)
                    throw Failure{selector + " takes a whole order", range};
                if (selector == "besselY:" && !(number.number > 0.0))
                    throw Failure{"besselY: takes a positive receiver", range};
                const auto n = static_cast<int>(order.number);
                return finite(Value::of(selector == "besselJ:" ? ::jn(n, number.number) : ::yn(n, number.number)));
            }
            if (selector == "even" || selector == "odd")
                return Value::of((std::fmod(std::abs(number.number), 2.0) == 0.0) == (selector == "even"));
            if (selector == "raisedTo:" && arguments[0].kind == Value::Kind::Number
                && arguments[0].unit == Unit::None)
            {
                return finite(Value::of(std::pow(number.number, arguments[0].number)));
            }
            // 1 to: 16 is an Interval, its values in an Array here, so that
            // (1 to: 16) collect: [...] makes one value for each.
            if (selector == "to:" || selector == "to:by:")
            {
                const auto step = selector == "to:by:" ? arguments[1] : Value::of(1.0);
                if (arguments[0].kind != Value::Kind::Number || step.kind != Value::Kind::Number || step.number == 0.0)
                    throw Failure{"An Interval needs numbers and a nonzero step", range};
                std::vector<Value> values;
                for (auto index = number.number;
                     step.number > 0.0 ? index <= arguments[0].number : index >= arguments[0].number;
                     index += step.number)
                {
                    if (values.size() >= maximumElements)
                        throw Failure{"An Interval exceeds the fixed element limit", range};
                    values.push_back(Value::of(index));
                }
                return Value::array(std::move(values));
            }
            if (selector == "**" && arguments[0].kind == Value::Kind::Number && arguments[0].unit == Unit::None)
                return finite(Value::of(std::pow(number.number, arguments[0].number), number.unit));
            // timesRepeat: answers nil.
            if (selector == "timesRepeat:")
            {
                for (double count = 0.0; count < number.number; count += 1.0)
                    callBlock(arguments[0], {}, range);
                return Value::nil();
            }
            // floatTo:do: and floatTo:by:do: are the same loops, numbers
            // here being doubles.
            if (selector == "to:do:" || selector == "to:by:do:" || selector == "floatTo:do:"
                || selector == "floatTo:by:do:")
            {
                const auto stepped = selector == "to:by:do:" || selector == "floatTo:by:do:";
                const auto step = stepped ? arguments[1] : Value::of(1.0);
                const auto& block = arguments[stepped ? 2 : 1];
                if (arguments[0].kind != Value::Kind::Number || step.kind != Value::Kind::Number
                    || step.number == 0.0)
                {
                    throw Failure{"A loop needs numbers and a nonzero step", range};
                }
                for (auto index = number.number;
                     step.number > 0.0 ? index <= arguments[0].number : index >= arguments[0].number;
                     index += step.number)
                {
                    const std::array argument{Value::of(index)};
                    callBlock(block, argument, range);
                }
                return number;
            }
        }
        // quo: divides toward zero and rem: is its remainder, as //
        // divides toward negative infinity and \\ is its;
        // mod: is \\.
        if (selector == "mod:")
        {
            const auto divisor = numberOf(arguments[0]);
            if (divisor.kind != Value::Kind::Number || divisor.unit != Unit::None || number.unit != Unit::None)
                throw Failure{selector + " takes numbers without units", range};
            if (divisor.number == 0.0)
                throw Failure{"Division by zero", range};
            return finite(Value::of(number.number - std::floor(number.number / divisor.number) * divisor.number));
        }
        if (selector == "quo:" || selector == "rem:")
        {
            const auto divisor = numberOf(arguments[0]);
            if (divisor.kind != Value::Kind::Number || divisor.unit != Unit::None || number.unit != Unit::None)
                throw Failure{selector + " takes numbers without units", range};
            if (divisor.number == 0.0)
                throw Failure{"Division by zero", range};
            const auto quotient = std::trunc(number.number / divisor.number);
            return finite(Value::of(selector == "quo:" ? quotient : number.number - quotient * divisor.number));
        }
        // vmax: and vmin: are max: and min:.
        if (selector == "min:" || selector == "max:" || selector == "vmin:" || selector == "vmax:")
        {
            const auto& other = arguments[0];
            if (other.kind != Value::Kind::Number || quantity(other.unit) != quantity(number.unit))
                throw Failure{"These values cannot be compared", range};
            const auto keepSelf = selector == "min:" || selector == "vmin:" ? comparable(number) <= comparable(other)
                                                                            : comparable(number) >= comparable(other);
            return keepSelf ? number : other;
        }
        // 36.7 roundTo: 1 is the nearest multiple of 1, and
        // truncateTo: the multiple toward zero, in the receiver's units.
        // Val's maxAbs: and minAbs:, whichever
        // has the greater or lesser magnitude, keeping its sign.
        if ((selector == "maxAbs:" || selector == "minAbs:") && arguments.size() == 1)
        {
            const auto other = numberOf(arguments[0]);
            if (other.kind != Value::Kind::Number || other.unit != number.unit)
                throw Failure{selector + " takes a number in the receiver's units", range};
            const auto otherWins = selector == "maxAbs:" ? std::abs(other.number) > std::abs(number.number)
                                                         : std::abs(other.number) < std::abs(number.number);
            return otherWins ? other : number;
        }
        if (selector == "roundTo:" || selector == "truncateTo:" || selector == "ceilingTo:")
        {
            const auto step = numberOf(arguments[0]);
            if (step.kind != Value::Kind::Number || step.unit != Unit::None || !std::isfinite(step.number)
                || step.number == 0.0)
            {
                throw Failure{selector + " takes a number other than 0", range};
            }
            const auto multiples = number.number / step.number;
            return Value::of((selector == "roundTo:"      ? std::round(multiples)
                                 : selector == "truncateTo:" ? std::trunc(multiples)
                                                             : std::ceil(multiples))
                    * step.number,
                number.unit);
        }
        if (selector == "between:and:")
        {
            const auto value = comparable(number);
            return Value::of(value >= comparable(arguments[0]) && value <= comparable(arguments[1]));
        }
        // Messages to Integers: shifts and the bits
        // and bytes of two's complement, primes, factorials, and greatest
        // common divisors and least common multiples.
        if (selector == "<<" || selector == ">>" || selector == "bitShift:" || selector == "bitAnd:"
            || selector == "bitOr:" || selector == "bitXor:" || selector == "bitInvert" || selector == "bottom24"
            || selector == "bottom32" || selector == "bottom48" || selector == "top24" || selector == "byte0"
            || selector == "byte1" || selector == "byte2" || selector == "byte3" || selector == "isPrime"
            || selector == "factorial" || selector == "gcd:" || selector == "lcm:")
        {
            const auto integer = [&](const Value& value) {
                if (value.kind != Value::Kind::Number || value.unit != Unit::None || value.number != std::floor(value.number)
                    || std::abs(value.number) > 9007199254740992.0)
                    throw Failure{selector + " takes integers", range};
                return static_cast<std::int64_t>(value.number);
            };
            const auto receiverInteger = integer(number);
            const auto answer = [&](std::int64_t value) {
                if (value > 9007199254740992LL || value < -9007199254740992LL)
                    throw Failure{selector + " answers an integer beyond what a script holds", range};
                return Value::of(static_cast<double>(value));
            };
            if (selector == "<<" || selector == ">>" || selector == "bitShift:")
            {
                auto shift = integer(arguments[0]);
                if (selector == ">>")
                    shift = -shift;
                if (std::abs(shift) > 62)
                    throw Failure{selector + " shifts by at most 62 bits", range};
                return answer(shift >= 0 ? receiverInteger * (std::int64_t{1} << shift) : receiverInteger >> -shift);
            }
            if (selector == "bitAnd:")
                return answer(receiverInteger & integer(arguments[0]));
            if (selector == "bitOr:")
                return answer(receiverInteger | integer(arguments[0]));
            if (selector == "bitXor:")
                return answer(receiverInteger ^ integer(arguments[0]));
            if (selector == "bitInvert")
                return answer(~receiverInteger);
            if (selector.starts_with("bottom"))
            {
                const auto bits = selector == "bottom24" ? 24 : selector == "bottom32" ? 32 : 48;
                return answer(receiverInteger & ((std::int64_t{1} << bits) - 1));
            }
            if (selector == "top24")
                return answer((receiverInteger >> 24) & 0xFFFFFF);
            if (selector.starts_with("byte"))
                return answer((receiverInteger >> (8 * (selector.back() - '0'))) & 0xFF);
            if (selector == "isPrime")
            {
                if (receiverInteger < 2)
                    return Value::of(false);
                for (std::int64_t divisor = 2; divisor * divisor <= receiverInteger; ++divisor)
                {
                    if (receiverInteger % divisor == 0)
                        return Value::of(false);
                }
                return Value::of(true);
            }
            if (selector == "factorial")
            {
                if (receiverInteger < 0 || receiverInteger > 18)
                    throw Failure{"factorial takes an integer from 0 to 18 here", range};
                std::int64_t product = 1;
                for (std::int64_t factor = 2; factor <= receiverInteger; ++factor)
                    product *= factor;
                return answer(product);
            }
            const auto other = integer(arguments[0]);
            const auto divisor = std::gcd(receiverInteger, other);
            if (selector == "gcd:")
                return answer(divisor);
            return answer(divisor == 0 ? 0 : std::abs(receiverInteger / divisor * other));
        }
        if (selector == "printString" || selector == "asString")
            return Value::string(fieldText(number).value_or(std::string{}));
        // An integer written in another base.
        if (selector == "printStringRadix:")
        {
            const auto& base = arguments[0];
            if (number.unit != talas::expr::Unit::None || number.number != std::floor(number.number)
                || std::abs(number.number) > 9007199254740992.0 || base.kind != Value::Kind::Number
                || base.number != std::floor(base.number) || base.number < 2.0 || base.number > 36.0)
            {
                throw Failure{"printStringRadix: takes an integer and a base from 2 to 36", range};
            }
            auto remaining = static_cast<std::uint64_t>(std::abs(number.number));
            const auto radix = static_cast<std::uint64_t>(base.number);
            std::string digits;
            do
            {
                const auto digit = static_cast<int>(remaining % radix);
                digits.insert(digits.begin(), static_cast<char>(digit < 10 ? '0' + digit : 'A' + digit - 10));
                remaining /= radix;
            } while (remaining != 0);
            return Value::string((number.number < 0.0 ? "-" : "") + digits);
        }
        throw Failure{"A number does not understand " + selector, range};
    }

    // Arithmetic with a value that changes as the Sound plays builds the
    // text of its expression; with a ?variable's declared value, the
    // script knows its value too.
    [[nodiscard]] Value combine(const Value& left, const std::string& selector, const Value& right,
        const SourceRange& range) const
    {
        const auto arithmeticSelector = selector == "+" || selector == "-" || selector == "*" || selector == "/"
            || selector == "//" || selector == "\\\\";
        const auto comparison = selector == "<" || selector == ">" || selector == "<=" || selector == ">=";
        if (!arithmeticSelector && !comparison && selector != "**")
            throw Failure{"An Event Value expression does not understand " + selector, range};
        const auto text = [](const Value& value) {
            return value.kind == Value::Kind::Expression ? "(" + value.text + ")" : *fieldText(value);
        };
        auto result = Value::expression(text(left) + " " + selector + " " + text(right));
        const auto knows = [](const Value& value) {
            return value.kind == Value::Kind::Number || (value.kind == Value::Kind::Expression && value.known);
        };
        if (arithmeticSelector && knows(left) && knows(right))
        {
            const auto value = arithmetic(numberOf(left), selector, numberOf(right), range);
            if (std::isfinite(value.number))
            {
                result.known = true;
                result.number = value.number;
                result.unit = value.unit;
            }
        }
        return result;
    }

    Value sendExpression(const Value& expression, const std::string& selector, std::span<const Value> arguments,
        const SourceRange& range)
    {
        const auto binary = !selector.empty() && !isLetter(selector.front());
        if (binary && arguments.size() == 1
            && (arguments[0].kind == Value::Kind::Number || arguments[0].kind == Value::Kind::Expression))
        {
            // A ?variable compares as the script knows it.
            if (expression.known && (arguments[0].kind == Value::Kind::Number || arguments[0].known)
                && (selector == "<" || selector == ">" || selector == "<=" || selector == ">="))
            {
                return sendNumber(numberOf(expression), selector, arguments, range);
            }
            return combine(expression, selector, arguments[0], range);
        }
        if (selector == "negated")
        {
            auto result = Value::expression("0 - (" + expression.text + ")");
            result.known = expression.known;
            result.number = -expression.number;
            result.unit = expression.unit;
            return result;
        }
        if (arguments.empty())
        {
            // A unary message, as a unit or smoothed, is Val's message
            // to the expression; its value is known
            // where the script knows the receiver's and the message is one
            // to numbers here.
            auto result = Value::expression("(" + expression.text + ") " + selector);
            if (expression.known)
            {
                try
                {
                    const auto value = sendNumber(numberOf(expression), selector, {}, range);
                    if (value.kind == Value::Kind::Number)
                    {
                        result.known = true;
                        result.number = value.number;
                        result.unit = value.unit;
                    }
                }
                catch (const Failure&)
                {
                }
            }
            return result;
        }
        // A keyword message: to the number the script knows, or Val's
        // message to a value that changes as the Sound plays.
        if (expression.known)
            return sendNumber(numberOf(expression), selector, arguments, range);
        std::string text = "(" + expression.text + ")";
        auto keywords = std::string_view{selector};
        for (const auto& argument : arguments)
        {
            const auto colon = keywords.find(':');
            auto argumentText = argument.kind == Value::Kind::Expression ? "(" + argument.text + ")"
                : argument.kind == Value::Kind::Number                       ? "(" + *fieldText(argument) + ")"
                                                                             : std::string{};
            // A collection of numbers is Val's Array of them, as of:
            // pattern is.
            if (argumentText.empty() && (argument.kind == Value::Kind::Array || argument.kind == Value::Kind::Object))
            {
                std::string array = "#(";
                for (const auto& element : elementsOf(argument, range))
                {
                    if (element.kind != Value::Kind::Number || element.unit != Unit::None)
                        throw Failure{"An Event Value expression takes Arrays of numbers alone", range};
                    array += (array.size() > 2 ? " " : "") + *fieldText(element);
                }
                argumentText = array + ")";
            }
            if (colon == std::string_view::npos || argumentText.empty())
                throw Failure{"An Event Value expression does not understand " + selector, range};
            text += " ";
            text += keywords.substr(0, colon + 1);
            text += " " + argumentText;
            keywords.remove_prefix(colon + 1);
        }
        return Value::expression(std::move(text));
    }

    // Random newForTalasWithSeed: 1002 is a stream of random numbers from 0
    // to 1, one for each next; a seed makes it repeatable. Random new
    // starts from seed 0, and seed: starts it again. The stream is Park and
    // Miller's minimal standard generator.
    static std::uint32_t seedOf(const Value& value, const SourceRange& range)
    {
        const auto number = numberOf(value);
        if (number.kind != Value::Kind::Number || number.unit != Unit::None || number.number < 0.0
            || number.number > 2147483645.0 || number.number != std::floor(number.number))
        {
            throw Failure{"A random seed is a whole number that is not negative", range};
        }
        // The generator's state lies in 1 to 2^31 - 2.
        return static_cast<std::uint32_t>(number.number) + 1u;
    }

    // A new object of a kind a script changes in place, in the slot of one
    // no Value holds any more once the kind's slots are all made, so a
    // Session that runs for hours, as a Tool's does, keeps only the objects
    // it can still reach.
    template <typename Object>
    Value adopt(std::vector<Object>& objects, std::vector<std::weak_ptr<const std::vector<Value>>>& held,
        Object object, std::size_t first, std::size_t limit, std::string_view what, const SourceRange& range)
    {
        auto token = std::make_shared<const std::vector<Value>>();
        auto slot = objects.size();
        if (slot >= limit)
        {
            const auto dead = std::ranges::find_if(held, [](const auto& one) { return one.expired(); });
            if (dead == held.end())
                throw Failure{"A script holds at most " + std::to_string(limit) + " " + std::string{what} + " at once", range};
            slot = static_cast<std::size_t>(dead - held.begin());
            objects[slot] = std::move(object);
            held[slot] = token;
        }
        else
        {
            objects.push_back(std::move(object));
            held.push_back(token);
        }
        auto value = Value::object(first + slot);
        value.elements = std::move(token);
        return value;
    }

    Value newSamplesFile(SampleFileInfo file, const SourceRange& range)
    {
        return adopt(samplesFiles_, samplesFilesHeld_, std::move(file), firstSamplesFile, maximumSamplesFiles,
            "SamplesFiles", range);
    }

    // What a SamplesFile's header says, and its
    // length; a file the host does not know evaluates the cancel block.
    Value sendSamplesFile(const Value& receiver, const std::string& selector, std::span<const Value> arguments,
        const SourceRange& range)
    {
        if (receiver.index == samplesFileClass)
        {
            if (selector == "fromOldFileNamed:ifCancelled:" && arguments.size() == 2)
            {
                if (arguments[0].kind != Value::Kind::String && arguments[0].kind != Value::Kind::Symbol)
                    throw Failure{selector + " takes a file name", range};
                auto found = host_.sampleFile(arguments[0].text);
                return found.has_value() ? newSamplesFile(std::move(*found), range) : callBlock(arguments[1], {}, range);
            }
            throw Failure{"SamplesFile understands fromOldFileNamed:ifCancelled:", range};
        }
        const auto file = samplesFiles_[receiver.index - firstSamplesFile];
        // The file located again, the user asked
        // for another where it cannot be found, the block's value on Cancel.
        if (selector == "locateFileIfCancelled:")
        {
            if (auto again = host_.sampleFile(file.name))
                return newSamplesFile(std::move(*again), range);
            const auto answer = askHost({PromptKind::OpenFile, "Locate " + file.name, {}, extensionsOfType(Value::string("Smpl"), range)}, range);
            if (answer.accepted)
                if (auto chosen = host_.sampleFile(answer.text))
                    return newSamplesFile(std::move(*chosen), range);
            return callBlock(arguments[0], {}, range);
        }
        // The samples, -1 to 1, of a mono file from a
        // start for a length; nil where they cannot be read.
        if (selector == "getSamplesFromMonoWavetableFileStartingAt:length:")
        {
            const auto count = [&](const Value& value) -> std::optional<std::size_t> {
                if (value.kind != Value::Kind::Number || !(value.number >= 0.0) || value.number != std::floor(value.number)
                    || value.number > static_cast<double>(maximumElements))
                    return std::nullopt;
                return static_cast<std::size_t>(value.number);
            };
            const auto start = count(arguments[0]);
            const auto length = count(arguments[1]);
            const auto samples = start && length && file.monoSamples ? file.monoSamples(*start, *length) : std::nullopt;
            if (!samples.has_value())
                return Value::nil();
            std::vector<Value> values;
            values.reserve(samples->size());
            for (const auto sample : *samples)
                values.push_back(Value::of(sample));
            return Value::array(std::move(values));
        }
        if (!arguments.empty())
            throw Failure{"A SamplesFile does not understand " + selector, range};
        if (selector == "filename")
            return newFilename(file.name, range);
        if (selector == "sampleFrames")
            return Value::of(file.frames);
        if (selector == "fileDuration")
            return Value::of(file.sampleRate > 0.0 ? file.frames / file.sampleRate : 0.0);
        if (selector == "uiSampleRate")
            return Value::of(file.sampleRate);
        if (selector == "hasLoop")
            return Value::of(file.loop.has_value());
        if (selector == "uiLoopStart")
            return Value::of(file.loop.has_value() ? static_cast<double>(file.loop->startFrame) : 0.0);
        if (selector == "uiLoopEnd")
        {
            return Value::of(file.loop.has_value() ? static_cast<double>(file.loop->endFrameExclusive) - 1.0
                                                   : std::max(0.0, file.frames - 1.0));
        }
        // A header without a pitch is middle C's, without a key range all
        // the keys' and velocities'.
        if (selector == "uiBasePitch")
        {
            return Value::of(file.pitch.has_value()
                    ? static_cast<double>(file.pitch->midiUnityNote)
                        + static_cast<double>(file.pitch->midiPitchFraction) / 4294967296.0
                    : 60.0,
                Unit::NoteNumber);
        }
        if (selector == "uiLowPitch" || selector == "uiHighPitch")
        {
            const auto keys = file.keys.value_or(talas::core::SampleKeyRange{});
            return Value::of(selector == "uiLowPitch" ? keys.lowNote : keys.highNote, Unit::NoteNumber);
        }
        if (selector == "uiLowVelocity" || selector == "uiHighVelocity")
        {
            const auto keys = file.keys.value_or(talas::core::SampleKeyRange{});
            return Value::of(static_cast<double>(selector == "uiLowVelocity" ? keys.lowVelocity : keys.highVelocity) / 127.0);
        }
        if (selector == "fileExistsAndIsValid")
            return Value::of(true);
        // The recording played at the current
        // sample rate, by a host that plays, as a Tool's environment.
        if (selector == "play")
        {
            if (trial_)
                throw TrialStop{true};
            if (!host_.playSampleFile(file.name))
                throw Failure{"Nothing here plays " + file.name + ": a Tool plays a SamplesFile", range};
            return receiver;
        }
        // The markers' names as Symbols, each giving
        // its frame; a name that comes twice keeps its first.
        if (selector == "markers")
        {
            Dictionary answer;
            for (const auto& marker : file.markers)
            {
                auto key = Value::string(marker.name);
                key.kind = Value::Kind::Symbol;
                if (std::ranges::none_of(answer.entries, [&](const auto& entry) { return entry.first.text == key.text; }))
                    answer.entries.emplace_back(std::move(key), Value::of(static_cast<double>(marker.frame)));
            }
            return adopt(dictionaries_, dictionariesHeld_, std::move(answer), firstDictionary, maximumCollections,
                "dictionaries", range);
        }
        throw Failure{"A SamplesFile does not understand " + selector, range};
    }

    // A value that never changes, carried in the Value itself: the index
    // names its class and the elements hold its parts. A Session that runs
    // for hours, as a Tool's does, makes as many as it likes.
    [[nodiscard]] static Value tagged(std::size_t tag, std::vector<Value> parts)
    {
        auto value = Value::array(std::move(parts));
        value.kind = Value::Kind::Object;
        value.index = tag;
        return value;
    }

    [[nodiscard]] static const std::vector<Value>* partsOf(const Value& value, std::size_t tag) noexcept
    {
        return value.kind == Value::Kind::Object && value.index == tag && value.elements != nullptr
            ? value.elements.get()
            : nullptr;
    }

    [[nodiscard]] std::optional<std::string> filenameOf(const Value& value) const
    {
        const auto* parts = partsOf(value, filenameTag);
        if (parts == nullptr)
            return std::nullopt;
        return (*parts)[0].text;
    }

    Value newFilename(std::string path, const SourceRange& range)
    {
        (void)range;
        return tagged(filenameTag, {Value::string(std::move(path))});
    }

    // A path's extension with its period, as .aif, or nil.
    static Value extensionOf(std::string_view path)
    {
        const auto tail = path.substr(path.find_last_of('/') + 1);
        const auto dot = tail.find_last_of('.');
        return dot == std::string_view::npos || dot == 0 ? Value::nil() : Value::string(std::string{tail.substr(dot)});
    }

    // A Filename's parts and the names it makes.
    // Scripts do not read, write, move or delete files here: a project's
    // script runs as its Sound compiles, and plays alike on every computer.
    Value sendFilename(const std::string& path, const std::string& selector, std::span<const Value> arguments,
        const SourceRange& range)
    {
        const auto slash = path.find_last_of('/');
        const auto head = slash == std::string::npos ? std::string{} : path.substr(0, slash == 0 ? 1 : slash);
        const auto tail = slash == std::string::npos ? path : path.substr(slash + 1);
        const auto within = [&](const Value& name) {
            if (name.kind != Value::Kind::String && name.kind != Value::Kind::Symbol)
                throw Failure{selector + " takes a String", range};
            return path.empty() ? name.text : path.ends_with('/') ? path + name.text : path + "/" + name.text;
        };
        if (arguments.empty())
        {
            if (selector == "asString" || selector == "asDisplayString")
                return Value::string(path);
            if (selector == "asFilename")
                return newFilename(path, range);
            if (selector == "displayHead")
                return Value::string(head);
            if (selector == "displayTail")
                return Value::string(tail);
            if (selector == "displayTailWithoutFileExtension")
            {
                const auto dot = tail.find_last_of('.');
                return Value::string(dot == std::string::npos || dot == 0 ? tail : tail.substr(0, dot));
            }
            if (selector == "fileExtension")
                return extensionOf(path);
            if (selector == "separator")
                return Value::string("/");
            if (selector == "directory")
                return newFilename(head, range);
        }
        if (arguments.size() == 1 && selector == "construct:")
            return newFilename(within(arguments[0]), range);
        if (arguments.size() == 1 && selector == "constructString:")
            return Value::string(within(arguments[0]));
        if (trial_)
            throw TrialStop{true};
        if (auto* files = host_.files())
            return sendFile(*files, path, head, selector, arguments, range);
        throw Failure{"A Filename here answers its parts and the names it makes, not " + selector, range};
    }

    // The file types HostDriverInterface answers,
    // by their Macintosh type codes, and the extensions that
    // mark files of each here, where a file has no type of its own.
    static const std::vector<std::pair<std::string, std::vector<std::string>>>& fileTypes()
    {
        static const std::vector<std::pair<std::string, std::vector<std::string>>> types{
            {"AIFF", {"aif", "aiff", "aifc"}}, {"WAVE", {"wav", "wave"}},
            {"Smpl", {"aif", "aiff", "aifc", "wav", "wave"}}, {"Midi", {"mid", "midi"}}, {"TEXT", {"txt", "text"}},
            {"GIFf", {"gif"}}, {"PNGf", {"png"}}, {"Spec", {"spc"}}};
        return types;
    }

    [[nodiscard]] static const std::vector<std::string>& extensionsOfType(const Value& type, const SourceRange& range)
    {
        const auto& types = fileTypes();
        const auto found = std::ranges::find(types, type.text, &std::pair<std::string, std::vector<std::string>>::first);
        if ((type.kind != Value::Kind::String && type.kind != Value::Kind::Symbol) || found == types.end())
            throw Failure{"There is no file type " + type.text, range};
        return found->second;
    }

    [[nodiscard]] static std::string lowered(std::string text)
    {
        std::ranges::transform(text, text.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        return text;
    }

    [[nodiscard]] static std::string extensionText(std::string_view name)
    {
        const auto slash = name.find_last_of('/');
        const auto tail = slash == std::string_view::npos ? name : name.substr(slash + 1);
        const auto dot = tail.find_last_of('.');
        return dot == std::string_view::npos || dot == 0 ? std::string{} : lowered(std::string{tail.substr(dot + 1)});
    }

    // A path from a Filename or a String.
    [[nodiscard]] std::string pathOf(const Value& value, const std::string& selector, const SourceRange& range) const
    {
        if (const auto path = filenameOf(value))
            return *path;
        if (value.kind == Value::Kind::String || value.kind == Value::Kind::Symbol)
            return value.text;
        throw Failure{selector + " takes a Filename or a String", range};
    }

    // A Filename's file, read whole, sized,
    // made, copied, moved, renamed and deleted; its directory listed; and
    // streams that read it, write it afresh, append to it, or both.
    Value sendFile(FileSystem& files, const std::string& path, const std::string& head, const std::string& selector,
        std::span<const Value> arguments, const SourceRange& range)
    {
        const auto self = newFilename(path, range);
        const auto contents = files.read(path);
        const auto existing = [&] {
            if (!contents.has_value())
                throw Failure{"There is no file " + path, range};
            return *contents;
        };
        const auto names = [&](const std::string& directory) {
            if (!files.isDirectory(directory))
                throw Failure{(directory.empty() ? std::string{"The top"} : directory) + " is no directory", range};
            return files.list(directory);
        };
        const auto strings = [](const std::vector<std::string>& texts) {
            std::vector<Value> values;
            for (const auto& text : texts)
                values.push_back(Value::string(text));
            return Value::array(std::move(values));
        };
        if (arguments.empty())
        {
            if (selector == "contentsOfEntireFile")
                return Value::string(existing());
            // ReadGIF answers the GIF image found in
            // the file, its width and height as its logical screen gives
            // them.
            if (selector == "readGIF")
            {
                const auto bytes = existing();
                if (bytes.size() < 10 || (bytes.compare(0, 6, "GIF87a") != 0 && bytes.compare(0, 6, "GIF89a") != 0))
                    throw Failure{"There is no GIF image in " + path, range};
                const auto word = [&](std::size_t at) {
                    return static_cast<double>(static_cast<unsigned char>(bytes[at])
                        | (static_cast<unsigned>(static_cast<unsigned char>(bytes[at + 1])) << 8U));
                };
                return tagged(imageTag, {Value::of(word(6)), Value::of(word(8))});
            }
            // When the file was last read, written
            // and changed, each as a Date and a Time, by #accessed,
            // #modified and #statusChanged.
            if (selector == "dates")
            {
                (void)existing();
                const auto dates = files.dates(path);
                if (!dates.has_value())
                    return Value::nil();
                Dictionary answer;
                for (const auto& [name, seconds] : {std::pair{"accessed", dates->accessed}, std::pair{"modified", dates->modified},
                         std::pair{"statusChanged", dates->statusChanged}})
                {
                    auto key = Value::string(name);
                    key.kind = Value::Kind::Symbol;
                    const auto [day, second] = localDayAndSecond(static_cast<std::int64_t>(std::floor(seconds)));
                    answer.entries.emplace_back(std::move(key),
                        Value::array({newMoment(false, day, range), newMoment(true, second, range)}));
                }
                return adopt(dictionaries_, dictionariesHeld_, std::move(answer), firstDictionary, maximumCollections,
                    "dictionaries", range);
            }
            if (selector == "fileSize")
                return Value::of(static_cast<double>(existing().size()));
            if (selector == "isDirectory")
                return Value::of(files.isDirectory(path));
            if (selector == "reallyExists")
                return Value::of(contents.has_value());
            if (selector == "reallyExistsOrIsDirectory")
                return Value::of(contents.has_value() || files.isDirectory(path));
            if (selector == "reallyWritable")
                return Value::of(!files.isDirectory(path));
            if (selector == "createIfNonExistent")
            {
                if (!contents.has_value())
                    files.write(path, std::string{});
                return self;
            }
            if (selector == "makeDirectory")
            {
                files.makeDirectory(path);
                return self;
            }
            if (selector == "delete")
            {
                if (!files.remove(path))
                    throw Failure{"There is no file " + path + " to delete", range};
                return self;
            }
            if (selector == "directoryContents")
                return strings(names(path));
            if (selector == "fileNamesInSameDirectory")
            {
                std::vector<Value> found;
                for (const auto& name : names(head))
                {
                    const auto full = head.empty() ? name : head.ends_with('/') ? head + name : head + "/" + name;
                    if (!files.isDirectory(full))
                        found.push_back(newFilename(full, range));
                }
                return Value::array(std::move(found));
            }
            if (selector == "readStream" || selector == "writeStream" || selector == "readWriteStream"
                || selector == "appendStream" || selector == "readAppendStream")
                return fileStream(files, path, selector, range);
            // A text file to read, as a score.
            if (selector == "textFileStream")
                return fileStream(files, path, "readStream", range);
        }
        if (arguments.size() == 1)
        {
            if (selector == "copyTo:" || selector == "moveTo:" || selector == "renameTo:")
            {
                const auto destination = pathOf(arguments[0], selector, range);
                const auto copied = existing();
                files.write(destination, copied);
                if (selector != "copyTo:" && destination != path)
                    (void)files.remove(path);
                return self;
            }
            if (selector == "directoryContentsFilteredBy:")
            {
                const auto* filter = partsOf(arguments[0], fileTypeFilterTag);
                if (filter == nullptr)
                    throw Failure{"directoryContentsFilteredBy: takes a filter, a file type's asFileTypeFilter", range};
                std::vector<std::string> matching;
                for (const auto& name : names(path))
                {
                    const auto extension = extensionText(name);
                    const auto matches = std::ranges::any_of(*filter, [&](const Value& type) {
                        return std::ranges::find(extensionsOfType(type, range), extension)
                            != extensionsOfType(type, range).end();
                    });
                    if (matches)
                        matching.push_back(name);
                }
                return strings(matching);
            }
        }
        throw Failure{"A Filename does not understand " + selector, range};
    }

    Value fileStream(FileSystem& files, const std::string& path, const std::string& kind, const SourceRange& range)
    {
        if (files.isDirectory(path))
            throw Failure{path + " is a directory", range};
        auto contents = files.read(path);
        if (kind == "readStream" && !contents.has_value())
            throw Failure{"There is no file " + path, range};
        // A write stream deletes the file it finds.
        if (kind == "writeStream" || !contents.has_value())
        {
            contents = std::string{};
            files.write(path, std::string{});
        }
        std::vector<Value> elements;
        elements.reserve(contents->size());
        for (const auto character : *contents)
            elements.push_back(Value::string(std::string(1, character)));
        auto value = makeStream(std::move(elements), true, false, range);
        auto& stream = streams_[value.index - firstReadStream];
        stream.file = path;
        stream.reads = kind != "writeStream" && kind != "appendStream";
        stream.writes = kind != "readStream";
        stream.appending = kind == "appendStream" || kind == "readAppendStream";
        if (kind == "appendStream")
            stream.position = stream.elements.size();
        stream.lineEnd = "\n";
        return value;
    }

    // What a file's stream holds, written to its file.
    void save(ReadStream& stream) noexcept
    {
        auto* files = host_.files();
        if (!stream.file.has_value() || !stream.dirty || files == nullptr)
            return;
        try
        {
            std::string text;
            for (const auto& element : stream.elements)
            {
                if (element.kind == Value::Kind::Number)
                    text += static_cast<char>(static_cast<unsigned char>(element.number));
                else
                    text += element.text;
            }
            files->write(*stream.file, std::move(text));
            stream.dirty = false;
        }
        catch (...)
        {
        }
    }

public:
    // Every file stream's writes, in its file, as a run ends.
    void saveFiles() noexcept
    {
        for (auto& stream : streams_)
            save(stream);
    }

private:
    // A file's stream closed, committed, made bytes or characters,
    // given its line end, and asked where it reads and writes; a message it
    // has no access for refused.
    std::optional<Value> sendFileStream(ReadStream& stream, const Value& receiver, const std::string& selector,
        const SourceRange& range)
    {
        if (selector == "closed")
            return Value::of(stream.closed);
        if (stream.closed)
            throw Failure{"The stream on " + *stream.file + " is closed", range};
        if (selector == "close" || selector == "flush" || selector == "commit")
        {
            save(stream);
            stream.closed = selector == "close";
            return receiver;
        }
        if (selector == "isBinary" || selector == "isText")
            return Value::of(stream.text == (selector == "isText"));
        if (selector == "binary" || selector == "text")
        {
            const auto text = selector == "text";
            for (auto& element : stream.elements)
            {
                if (text && element.kind == Value::Kind::Number)
                    element = Value::string(std::string(1, static_cast<char>(static_cast<unsigned char>(element.number))));
                else if (!text && element.kind == Value::Kind::String && element.text.size() == 1)
                    element = Value::of(static_cast<double>(static_cast<unsigned char>(element.text.front())));
            }
            stream.text = text;
            return receiver;
        }
        if (selector == "lineEndCR" || selector == "lineEndCRLF" || selector == "lineEndLF"
            || selector == "lineEndTransparent")
        {
            stream.lineEnd = selector == "lineEndCR" ? "\r" : selector == "lineEndCRLF" ? "\r\n" : "\n";
            return receiver;
        }
        if (selector == "readPosition")
            return Value::of(static_cast<double>(stream.position));
        if (selector == "writePosition")
            return Value::of(static_cast<double>(stream.appending ? stream.elements.size() : stream.position));
        const auto writing = selector.ends_with("Put:") || selector == "nextPutAll:" || selector == "print:"
            || selector == "cr" || selector == "space" || selector == "tab" || selector == "padTo:";
        const auto neutral = selector == "contents" || selector == "position" || selector == "position:"
            || selector == "reset" || selector == "isEmpty" || selector == "size";
        if (writing && !stream.writes)
            throw Failure{*stream.file + " is open for reading only", range};
        if (!writing && !neutral && !stream.reads)
            throw Failure{*stream.file + " is open for writing only", range};
        return std::nullopt;
    }

    // HostDriverInterface: file types and their
    // lists, Filenames given a type's extension, and files copied; a file
    // here has no type or creator of its own to set.
    // Preferences locates a file by its name among
    // those the host reaches, as a Filename; nil when there is none.
    Value sendPreferences(const std::string& selector, std::span<const Value> arguments, const SourceRange& range)
    {
        if (selector != "locateFileNamed:ofType:locateMessage:")
            throw Failure{"Preferences understands locateFileNamed:ofType:locateMessage:", range};
        const auto& name = arguments[0];
        if (name.kind != Value::Kind::String && name.kind != Value::Kind::Symbol)
            throw Failure{selector + " takes a file's name", range};
        auto* files = host_.files();
        if (files == nullptr)
            return Value::nil();
        if (files->read(name.text).has_value())
            return newFilename(name.text, range);
        std::vector<std::string> directories{std::string{}};
        while (!directories.empty())
        {
            const auto directory = std::move(directories.back());
            directories.pop_back();
            for (const auto& entry : files->list(directory))
            {
                const auto full = directory.empty() ? entry : directory + "/" + entry;
                if (files->isDirectory(full))
                    directories.push_back(full);
                else if (entry == name.text)
                    return newFilename(full, range);
            }
        }
        return Value::nil();
    }

    Value sendHostDriverInterface(const std::string& selector, std::span<const Value> arguments, const SourceRange& range)
    {
        const std::array<std::pair<std::string_view, std::string_view>, 7> types{{{"aiffFileType", "AIFF"},
            {"waveFileType", "WAVE"}, {"sampleFileType", "Smpl"}, {"midiFileType", "Midi"}, {"textFileType", "TEXT"},
            {"gifFileType", "GIFf"}, {"pngFileType", "PNGf"}}};
        if (const auto found = std::ranges::find(types, selector, &std::pair<std::string_view, std::string_view>::first);
            arguments.empty() && found != types.end())
            return Value::string(std::string{found->second});
        const std::array<std::pair<std::string_view, std::vector<std::string_view>>, 4> lists{{
            {"sampleFileTypes", {"AIFF", "WAVE"}}, {"spectrumFileTypes", {"Spec"}}, {"midiFileTypes", {"Midi"}},
            {"textFileTypes", {"TEXT"}}}};
        if (const auto found = std::ranges::find(lists, selector, &std::pair<std::string_view, std::vector<std::string_view>>::first);
            arguments.empty() && found != lists.end())
        {
            std::vector<Value> listed;
            for (const auto type : found->second)
                listed.push_back(Value::string(std::string{type}));
            return Value::array(std::move(listed));
        }
        if (selector == "flushFileSystem" && arguments.empty())
        {
            saveFiles();
            return Value::nil();
        }
        if (selector == "createFilenameFromString:andType:" || selector == "createFilenameWithExtensionFromString:andType:")
        {
            if (arguments[0].kind != Value::Kind::String && arguments[0].kind != Value::Kind::Symbol)
                throw Failure{selector + " takes a String and a file type", range};
            const auto& extensions = extensionsOfType(arguments[1], range);
            auto name = arguments[0].text;
            if (selector == "createFilenameWithExtensionFromString:andType:"
                && std::ranges::find(extensions, extensionText(name)) == extensions.end())
                name += "." + extensions.front();
            return newFilename(name, range);
        }
        if (selector == "copyFile:to:")
        {
            auto* files = host_.files();
            if (files == nullptr)
                throw Failure{"This script reaches no files", range};
            const auto from = pathOf(arguments[0], selector, range);
            const auto contents = files->read(from);
            if (!contents.has_value())
                throw Failure{"There is no file " + from, range};
            files->write(pathOf(arguments[1], selector, range), *contents);
            return Value::nil();
        }
        if (selector == "copyFileTypeAndCreatorFrom:to:" || selector == "setFileTypeAndCreatorForAIFFSamplesFile:"
            || selector == "setFileTypeAndCreatorForMIDIFile:" || selector == "setFileTypeAndCreatorForSpectrumFile:"
            || selector == "setFileTypeAndCreatorForTextFile:" || selector == "setFileTypeAndCreatorForWAVESamplesFile:")
        {
            for (const auto& argument : arguments)
                (void)pathOf(argument, selector, range);
            return Value::nil();
        }
        throw Failure{"HostDriverInterface does not understand " + selector, range};
    }

    using Point = std::pair<double, double>;

    [[nodiscard]] std::optional<Point> pointOf(const Value& value) const noexcept
    {
        const auto* parts = partsOf(value, pointTag);
        if (parts == nullptr)
            return std::nullopt;
        return Point{(*parts)[0].number, (*parts)[1].number};
    }

    Value newPoint(Point point, const SourceRange& range)
    {
        if (!std::isfinite(point.first) || !std::isfinite(point.second))
            throw Failure{"The result is not finite", range};
        return tagged(pointTag, {Value::of(point.first), Value::of(point.second)});
    }

    Value pointArithmetic(Point left, const std::string& selector, Point right, const SourceRange& range)
    {
        if (selector == "/" && (right.first == 0.0 || right.second == 0.0))
            throw Failure{"Division by zero", range};
        const auto apply = [&](double a, double b) {
            return selector == "+" ? a + b : selector == "-" ? a - b : selector == "*" ? a * b : a / b;
        };
        return newPoint({apply(left.first, right.first), apply(left.second, right.second)}, range);
    }

    // A Point's coordinates, its distance from
    // 0@0 and its angle from 3 o'clock, from 0 up to 2 pi, and arithmetic
    // with Points and numbers.
    Value sendPoint(Point point, const std::string& selector, std::span<const Value> arguments, const SourceRange& range)
    {
        if (arguments.size() == 1 && (selector == "+" || selector == "-" || selector == "*" || selector == "/"))
        {
            if (const auto other = pointOf(arguments[0]))
                return pointArithmetic(point, selector, *other, range);
            const auto number = numberOf(arguments[0]);
            if (number.kind != Value::Kind::Number || number.unit != Unit::None)
                throw Failure{"A Point takes Points and numbers without units", range};
            return pointArithmetic(point, selector, {number.number, number.number}, range);
        }
        if (arguments.empty())
        {
            if (selector == "x")
                return Value::of(point.first);
            if (selector == "y")
                return Value::of(point.second);
            if (selector == "r")
                return Value::of(std::hypot(point.first, point.second));
            if (selector == "theta")
            {
                const auto angle = std::atan2(point.second, point.first);
                return Value::of(angle < 0.0 ? angle + 2.0 * std::numbers::pi : angle);
            }
            if (selector == "negated")
                return newPoint({-point.first, -point.second}, range);
            if (selector == "asFloatPoint" || selector == "asPoint")
                return newPoint(point, range);
            if (selector == "transpose")
                return newPoint({point.second, point.first}, range);
        }
        // 0 @ 0 corner: 100 @ 100 and 0 @ 0 extent: 100 @ 100 are Rectangles.
        if (arguments.size() == 1 && (selector == "corner:" || selector == "extent:"))
        {
            const auto other = pointOf(arguments[0]);
            if (!other.has_value())
                throw Failure{selector + " takes a Point", range};
            return selector == "corner:"
                ? newRectangle({point.first, point.second, other->first, other->second}, range)
                : newRectangle({point.first, point.second, point.first + other->first, point.second + other->second}, range);
        }
        throw Failure{"A Point does not understand " + selector, range};
    }

    // A Rectangle by its origin, the top left, and its corner, the bottom
    // right, y growing downwards as on the screen.
    using Rectangle = std::array<double, 4>;

    [[nodiscard]] static std::optional<Rectangle> rectangleOf(const Value& value) noexcept
    {
        const auto* parts = partsOf(value, rectangleTag);
        if (parts == nullptr)
            return std::nullopt;
        return Rectangle{(*parts)[0].number, (*parts)[1].number, (*parts)[2].number, (*parts)[3].number};
    }

    Value newRectangle(Rectangle rectangle, const SourceRange& range)
    {
        if (!std::ranges::all_of(rectangle, [](double side) { return std::isfinite(side); }))
            throw Failure{"The result is not finite", range};
        return tagged(rectangleTag, {Value::of(rectangle[0]), Value::of(rectangle[1]), Value::of(rectangle[2]),
            Value::of(rectangle[3])});
    }

    Value sendRectangleClass(const std::string& selector, std::span<const Value> arguments, const SourceRange& range)
    {
        const auto points = [&] {
            const auto first = pointOf(arguments[0]);
            const auto second = pointOf(arguments[1]);
            if (!first.has_value() || !second.has_value())
                throw Failure{selector + " takes two Points", range};
            return std::pair{*first, *second};
        };
        if (selector == "origin:corner:")
        {
            const auto [origin, corner] = points();
            return newRectangle({origin.first, origin.second, corner.first, corner.second}, range);
        }
        if (selector == "origin:extent:")
        {
            const auto [origin, extent] = points();
            return newRectangle({origin.first, origin.second, origin.first + extent.first, origin.second + extent.second},
                range);
        }
        if (selector == "left:right:top:bottom:")
        {
            std::array<double, 4> sides{};
            for (std::size_t index = 0; index < 4; ++index)
            {
                const auto side = numberOf(arguments[index]);
                if (side.kind != Value::Kind::Number || side.unit != Unit::None)
                    throw Failure{"left:right:top:bottom: takes numbers without units", range};
                sides[index] = side.number;
            }
            return newRectangle({sides[0], sides[2], sides[1], sides[3]}, range);
        }
        throw Failure{"Rectangle understands origin:corner:, origin:extent: and left:right:top:bottom:", range};
    }

    Value sendRectangle(Rectangle rectangle, const std::string& selector, std::span<const Value> arguments,
        const SourceRange& range)
    {
        const auto [left, top, right, bottom] = rectangle;
        if (arguments.empty())
        {
            if (selector == "origin" || selector == "topLeft")
                return newPoint({left, top}, range);
            if (selector == "corner" || selector == "bottomRight")
                return newPoint({right, bottom}, range);
            if (selector == "topRight")
                return newPoint({right, top}, range);
            if (selector == "bottomLeft")
                return newPoint({left, bottom}, range);
            if (selector == "extent")
                return newPoint({right - left, bottom - top}, range);
            if (selector == "center")
                return newPoint({(left + right) / 2.0, (top + bottom) / 2.0}, range);
            if (selector == "width")
                return Value::of(right - left);
            if (selector == "height")
                return Value::of(bottom - top);
            if (selector == "area")
                return Value::of((right - left) * (bottom - top));
            if (selector == "left" || selector == "top" || selector == "right" || selector == "bottom")
                return Value::of(selector == "left" ? left : selector == "top" ? top : selector == "right" ? right : bottom);
        }
        if (arguments.size() == 1)
        {
            if (selector == "containsPoint:")
            {
                const auto point = pointOf(arguments[0]);
                if (!point.has_value())
                    throw Failure{"containsPoint: takes a Point", range};
                return Value::of(point->first >= left && point->first < right && point->second >= top
                    && point->second < bottom);
            }
            // By a number or a Point inwards, outwards, or across.
            const auto amount = [&]() -> std::pair<double, double> {
                if (const auto point = pointOf(arguments[0]))
                    return *point;
                const auto number = numberOf(arguments[0]);
                if (number.kind != Value::Kind::Number || number.unit != Unit::None)
                    throw Failure{selector + " takes a number or a Point", range};
                return {number.number, number.number};
            };
            if (selector == "insetBy:" || selector == "expandBy:" || selector == "expandedBy:")
            {
                const auto [x, y] = amount();
                const auto sign = selector == "insetBy:" ? 1.0 : -1.0;
                return newRectangle({left + sign * x, top + sign * y, right - sign * x, bottom - sign * y}, range);
            }
            if (selector == "translatedBy:" || selector == "translateBy:")
            {
                const auto [x, y] = amount();
                return newRectangle({left + x, top + y, right + x, bottom + y}, range);
            }
        }
        throw Failure{"A Rectangle does not understand " + selector, range};
    }

    [[nodiscard]] std::optional<std::complex<double>> complexOf(const Value& value) const noexcept
    {
        const auto* parts = partsOf(value, complexTag);
        if (parts == nullptr)
            return std::nullopt;
        return std::complex<double>{(*parts)[0].number, (*parts)[1].number};
    }

    Value newComplex(std::complex<double> value, const SourceRange& range)
    {
        if (!std::isfinite(value.real()) || !std::isfinite(value.imag()))
            throw Failure{"The result is not finite", range};
        return tagged(complexTag, {Value::of(value.real()), Value::of(value.imag())});
    }

    Value complexArithmetic(std::complex<double> left, const std::string& selector, std::complex<double> right,
        const SourceRange& range)
    {
        if (selector == "/" && right == std::complex<double>{})
            throw Failure{"Division by zero", range};
        return newComplex(selector == "+" ? left + right : selector == "-" ? left - right
                : selector == "*"          ? left * right : left / right,
            range);
    }

    // A complex number's parts, angle, magnitude and
    // conjugate, its square root, and its arithmetic with numbers.
    Value sendComplex(std::complex<double> value, const std::string& selector, std::span<const Value> arguments,
        const SourceRange& range)
    {
        if (arguments.size() == 1 && (selector == "+" || selector == "-" || selector == "*" || selector == "/"))
        {
            if (const auto other = complexOf(arguments[0]))
                return complexArithmetic(value, selector, *other, range);
            const auto number = numberOf(arguments[0]);
            if (number.kind != Value::Kind::Number || number.unit != Unit::None)
                throw Failure{"A complex number takes numbers without units", range};
            return complexArithmetic(value, selector, {number.number, 0.0}, range);
        }
        if (arguments.empty())
        {
            if (selector == "real")
                return Value::of(value.real());
            if (selector == "imaginary")
                return Value::of(value.imag());
            if (selector == "angle")
                return Value::of(std::arg(value));
            if (selector == "abs")
                return Value::of(std::abs(value));
            if (selector == "magnitudeSquared")
                return Value::of(std::norm(value));
            if (selector == "conjugated")
                return newComplex(std::conj(value), range);
            if (selector == "negated")
                return newComplex(-value, range);
            if (selector == "complexSqrt" || selector == "sqrt")
                return newComplex(std::sqrt(value), range);
            if (selector == "isComplex")
                return Value::of(value.imag() != 0.0);
        }
        throw Failure{"A complex number does not understand " + selector, range};
    }

    // A Date counts its days from 1 January 1901,
    // a Time its seconds from midnight.
    [[nodiscard]] static bool dateOrTime(const Value& value) noexcept
    {
        return partsOf(value, dateTag) != nullptr || partsOf(value, timeTag) != nullptr;
    }

    [[nodiscard]] static bool isTime(const Value& value) noexcept
    {
        return partsOf(value, timeTag) != nullptr;
    }

    [[nodiscard]] static std::int64_t moment(const Value& value)
    {
        return static_cast<std::int64_t>((*value.elements)[0].number);
    }

    // Days from 1 January 1970 to the date (Howard Hinnant's algorithm), and
    // back.
    static std::int64_t civilDays(std::int64_t year, std::int64_t month, std::int64_t day) noexcept
    {
        year -= month <= 2 ? 1 : 0;
        const auto era = (year >= 0 ? year : year - 399) / 400;
        const auto yearOfEra = year - era * 400;
        const auto dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
        const auto dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
        return era * 146097 + dayOfEra - 719468;
    }

    struct Civil
    {
        std::int64_t year;
        std::int64_t month;
        std::int64_t day;
    };

    static Civil civilDate(std::int64_t days) noexcept
    {
        days += 719468;
        const auto era = (days >= 0 ? days : days - 146096) / 146097;
        const auto dayOfEra = days - era * 146097;
        const auto yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
        const auto dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
        const auto shifted = (5 * dayOfYear + 2) / 153;
        const auto day = dayOfYear - (153 * shifted + 2) / 5 + 1;
        const auto month = shifted < 10 ? shifted + 3 : shifted - 9;
        return {yearOfEra + era * 400 + (month <= 2 ? 1 : 0), month, day};
    }

    static bool leapYear(std::int64_t year) noexcept
    {
        return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    }

    static constexpr std::int64_t daysFrom1901 = 25202; // 1 January 1970 less 1 January 1901

    // The local day since 1 January 1901 and the second of that day of a
    // time in seconds since 1970.
    static std::pair<std::int64_t, std::int64_t> localDayAndSecond(std::int64_t clockSeconds) noexcept
    {
        const auto time = static_cast<std::time_t>(clockSeconds);
        std::tm local{};
        localtime_r(&time, &local);
        return {civilDays(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday) + daysFrom1901,
            static_cast<std::int64_t>(local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec)};
    }

    Value newMoment(bool time, std::int64_t count, const SourceRange& range)
    {
        (void)range;
        return tagged(time ? timeTag : dateTag, {Value::of(static_cast<double>(count))});
    }

    Value sendDateOrTime(const Value& receiver, const std::string& selector, std::span<const Value> arguments,
        const SourceRange& range)
    {
        const auto whole = [&](const Value& value) {
            const auto number = numberOf(value);
            if (number.kind != Value::Kind::Number || number.unit != Unit::None || number.number != std::floor(number.number)
                || std::abs(number.number) > 1.0e15)
                throw Failure{selector + " takes a whole number", range};
            return static_cast<std::int64_t>(number.number);
        };
        const auto now = std::chrono::system_clock::now();
        const auto clockSeconds = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
        const auto today = [&] { return localDayAndSecond(clockSeconds); };
        if (receiver.index == dateClass)
        {
            if (selector == "today" && arguments.empty())
                return newMoment(false, today().first, range);
            if (selector == "dateAndTimeNow" && arguments.empty())
            {
                const auto [day, second] = today();
                return Value::array({newMoment(false, day, range), newMoment(true, second, range)});
            }
            if (selector == "newDay:year:" && arguments.size() == 2)
                return newMoment(false, civilDays(whole(arguments[1]), 1, 1) + daysFrom1901 + whole(arguments[0]) - 1, range);
            throw Failure{"Date understands today, dateAndTimeNow and newDay:year:", range};
        }
        if (receiver.index == timeClass)
        {
            if (selector == "now" && arguments.empty())
                return newMoment(true, today().second, range);
            if (selector == "dateAndTimeNow" && arguments.empty())
            {
                const auto [day, second] = today();
                return Value::array({newMoment(false, day, range), newMoment(true, second, range)});
            }
            if (selector == "fromSeconds:" && arguments.size() == 1)
                return newMoment(true, whole(arguments[0]), range);
            if (selector == "millisecondClockValue" && arguments.empty())
            {
                static const auto started = std::chrono::steady_clock::now();
                return Value::of(static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started).count()));
            }
            throw Failure{"Time understands now, dateAndTimeNow, fromSeconds: and millisecondClockValue", range};
        }
        const auto time = isTime(receiver);
        const auto count = moment(receiver);
        const auto other = [&](bool date) {
            if (!dateOrTime(arguments[0]) || isTime(arguments[0]) == date)
                throw Failure{selector + " takes a " + std::string{date ? "Date" : "Time"}, range};
            return moment(arguments[0]);
        };
        if (arguments.size() == 1 && (selector == "<" || selector == ">" || selector == "<=" || selector == ">="))
        {
            const auto right = other(!time);
            return Value::of(selector == "<" ? count < right : selector == ">" ? count > right
                    : selector == "<=" ? count <= right : count >= right);
        }
        if (time)
        {
            if ((selector == "addTime:" || selector == "subtractTime:") && arguments.size() == 1)
            {
                if (!dateOrTime(arguments[0]))
                    throw Failure{selector + " takes a Time or a Date", range};
                const auto amount = isTime(arguments[0]) ? moment(arguments[0]) : moment(arguments[0]) * 86400;
                return newMoment(true, selector == "addTime:" ? count + amount : count - amount, range);
            }
            if (arguments.empty())
            {
                if (selector == "asSeconds")
                    return Value::of(static_cast<double>(count));
                if (selector == "hours")
                    return Value::of(static_cast<double>(count / 3600));
                if (selector == "minutes")
                    return Value::of(static_cast<double>(count % 3600 / 60));
                if (selector == "seconds")
                    return Value::of(static_cast<double>(count % 60));
            }
            throw Failure{"A Time does not understand " + selector, range};
        }
        const auto civil = civilDate(count - daysFrom1901);
        const auto firstOfYear = civilDays(civil.year, 1, 1) + daysFrom1901;
        const auto monthDays = [&](std::int64_t month) {
            constexpr std::array<std::int64_t, 12> lengths{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
            return lengths[static_cast<std::size_t>(month - 1)] + (month == 2 && leapYear(civil.year) ? 1 : 0);
        };
        const auto daysInYear = leapYear(civil.year) ? 366 : 365;
        if (selector == "subtractDate:" && arguments.size() == 1)
            return Value::of(static_cast<double>(count - other(true)));
        if ((selector == "addDays:" || selector == "subtractDays:") && arguments.size() == 1)
            return newMoment(false, selector == "addDays:" ? count + whole(arguments[0]) : count - whole(arguments[0]), range);
        if (arguments.empty())
        {
            if (selector == "asSeconds")
                return Value::of(static_cast<double>(count) * 86400.0);
            if (selector == "dayOfMonth")
                return Value::of(static_cast<double>(civil.day));
            if (selector == "daysInMonth")
                return Value::of(static_cast<double>(monthDays(civil.month)));
            if (selector == "day")
                return Value::of(static_cast<double>(count - firstOfYear + 1));
            if (selector == "firstDayOfMonth")
                return Value::of(static_cast<double>(civilDays(civil.year, civil.month, 1) + daysFrom1901 - firstOfYear + 1));
            if (selector == "daysLeftInYear")
                return Value::of(static_cast<double>(daysInYear - (count - firstOfYear + 1)));
            if (selector == "daysInYear")
                return Value::of(static_cast<double>(daysInYear));
            if (selector == "year")
                return Value::of(static_cast<double>(civil.year));
            if (selector == "monthIndex")
                return Value::of(static_cast<double>(civil.month));
            // 1 January 1901 was a Tuesday.
            if (selector == "weekdayIndex")
                return Value::of(static_cast<double>((count % 7 + 7 + 1) % 7 + 1));
        }
        throw Failure{"A Date does not understand " + selector, range};
    }

    Value sendRandom(const Value& receiver, const std::string& selector, std::span<const Value> arguments,
        const SourceRange& range)
    {
        const auto seed = [&](const Value& value) { return seedOf(value, range); };
        if (receiver.index == randomClass)
        {
            if ((selector == "newForTalasWithSeed:" && arguments.size() == 1) || selector == "new")
            {
                // Random new, unseeded, another sequence each run.
                const auto fresh = [&] { return 1U + host_.freshSeed() % 2147483646U; };
                return adopt(randoms_, randomsHeld_, selector == "new" ? fresh() : seed(arguments[0]), firstRandom,
                    maximumRandoms, "random number generators", range);
            }
            throw Failure{"Random understands newForTalasWithSeed: and new", range};
        }
        const auto index = receiver.index - firstRandom;
        if (index >= randoms_.size())
            throw Failure{"The object does not understand " + selector, range};
        if (selector == "next" && arguments.empty())
        {
            randoms_[index] = static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(randoms_[index]) * 16807u) % 2147483647u);
            return Value::of(static_cast<double>(randoms_[index] - 1u) / 2147483646.0);
        }
        if (selector == "seed:" && arguments.size() == 1)
        {
            randoms_[index] = seed(arguments[0]);
            return receiver;
        }
        throw Failure{"A Random understands next and seed:", range};
    }

    // OneOverF newForTalasWithSeed: 52 states: 128 is a stream of random
    // numbers from 0 to 1 that stay near one another for a while, then
    // jump: Voss's 1/f generator, the mean of one row
    // per binary digit of the states (7 for the default 128), the row of
    // the lowest digit that changes drawn again at each next from Park and
    // Miller's generator seeded as a Random is.
    Value sendOneOverF(const Value& receiver, const std::string& selector, std::span<const Value> arguments,
        const SourceRange& range)
    {
        const auto draw = [](std::uint32_t& state) {
            state = static_cast<std::uint32_t>((static_cast<std::uint64_t>(state) * 16807u) % 2147483647u);
            return static_cast<double>(state - 1u) / 2147483646.0;
        };
        if (receiver.index == oneOverFClass)
        {
            const auto made = selector == "new" || (selector == "newForTalasWithSeed:" && arguments.size() == 1)
                || (selector == "newForTalasWithSeed:states:" && arguments.size() == 2);
            if (!made)
                throw Failure{"OneOverF understands newForTalasWithSeed:, newForTalasWithSeed:states: and new", range};
            auto states = 128.0;
            if (arguments.size() == 2)
            {
                const auto given = numberOf(arguments[1]);
                if (given.kind != Value::Kind::Number || given.unit != Unit::None || given.number < 2.0
                    || given.number > 65536.0 || given.number != std::floor(given.number))
                {
                    throw Failure{"A OneOverF's states are a whole number from 2 to 65536", range};
                }
                states = given.number;
            }
            OneOverF generator;
            generator.state = 1u;
            if (!arguments.empty())
            {
                // The seed as a Random takes it.
                generator.state = seedOf(arguments[0], range);
            }
            const auto rows = static_cast<std::size_t>(std::ceil(std::log2(states)));
            for (std::size_t row = 0; row < rows; ++row)
                generator.rows.push_back(draw(generator.state));
            return adopt(oneOverFs_, oneOverFsHeld_, std::move(generator), firstOneOverF, maximumRandoms,
                "OneOverF generators", range);
        }
        auto& generator = oneOverFs_[receiver.index - firstOneOverF];
        if (selector == "next" && arguments.empty())
        {
            ++generator.count;
            const auto row = static_cast<std::size_t>(std::countr_zero(generator.count)) % generator.rows.size();
            generator.rows[row] = draw(generator.state);
            auto sum = 0.0;
            for (const auto value : generator.rows)
                sum += value;
            return Value::of(sum / static_cast<double>(generator.rows.size()));
        }
        throw Failure{"A OneOverF understands next", range};
    }

    // MarkovChain trainingSequence: anArray chainLength: 2 counts which
    // element follows each run of chainLength - 1 elements of the training
    // sequence, and how often; newSequenceOfLength: 100 seed: 9008 starts
    // from a run at a place of the training sequence drawn at random and
    // draws each next element by those counts, so that every chainLength
    // elements in a row occur in the training sequence.
    // When no element follows the last run, the sequence ends there,
    // shorter. Elements are alike as = finds them; the draws are
    // Park and Miller's generator seeded as a Random is.
    Value sendMarkovChain(const Value& receiver, const std::string& selector, std::span<const Value> arguments,
        const SourceRange& range)
    {
        const auto whole = [&](const Value& value, std::size_t most, const char* message) {
            const auto number = numberOf(value);
            if (number.kind != Value::Kind::Number || number.unit != Unit::None || number.number < 0.0
                || number.number > static_cast<double>(most) || number.number != std::floor(number.number))
            {
                throw Failure{message, range};
            }
            return static_cast<std::size_t>(number.number);
        };
        if (receiver.index == markovChainClass)
        {
            if (selector != "trainingSequence:chainLength:")
                throw Failure{"MarkovChain understands trainingSequence:chainLength:", range};
            const auto& training = arguments[0];
            if (training.kind != Value::Kind::Array || training.elements->empty())
                throw Failure{"A MarkovChain's training sequence is an Array that is not empty", range};
            const auto size = training.elements->size();
            const auto length = whole(arguments[1], std::min(size, maximumChainLength),
                "A MarkovChain's chainLength is a whole number from 1 to 64, and at most its training sequence's size");
            if (length == 0)
                throw Failure{"A MarkovChain's chainLength is a whole number from 1 to 64", range};
            if (markovChains_.size() >= maximumRandoms || markovTrainingElements_ + size > maximumMarkovTraining)
                throw Failure{"A script's MarkovChains hold at most 262144 training elements in all", range};
            MarkovChain chain;
            chain.order = length - 1;
            std::map<std::string, std::uint32_t> ids;
            for (const auto& element : *training.elements)
            {
                const auto [found, added] =
                    ids.try_emplace(elementKey(element), static_cast<std::uint32_t>(chain.elements.size()));
                if (added)
                    chain.elements.push_back(element);
                chain.training.push_back(found->second);
            }
            for (std::size_t at = 0; at + chain.order < size; ++at)
            {
                auto& following = chain.following[{chain.training.begin() + static_cast<std::ptrdiff_t>(at),
                    chain.training.begin() + static_cast<std::ptrdiff_t>(at + chain.order)}];
                const auto element = chain.training[at + chain.order];
                const auto counted = std::ranges::find(following, element, &std::pair<std::uint32_t, std::size_t>::first);
                if (counted == following.end())
                    following.emplace_back(element, 1);
                else
                    ++counted->second;
            }
            // Each run's counts made cumulative, for drawing by them.
            for (auto& [run, following] : chain.following)
            {
                for (std::size_t index = 1; index < following.size(); ++index)
                    following[index].second += following[index - 1].second;
            }
            markovTrainingElements_ += size;
            markovChains_.push_back(std::move(chain));
            return Value::object(firstMarkovChain + markovChains_.size() - 1);
        }
        if (selector != "newSequenceOfLength:seed:")
            throw Failure{"A MarkovChain understands newSequenceOfLength:seed:", range};
        const auto& chain = markovChains_[receiver.index - firstMarkovChain];
        const auto length =
            whole(arguments[0], maximumElements, "A MarkovChain's sequence length is a whole number from 0 to 65536");
        auto state = seedOf(arguments[1], range);
        const auto draw = [&state] {
            state = static_cast<std::uint32_t>((static_cast<std::uint64_t>(state) * 16807u) % 2147483647u);
            return static_cast<double>(state - 1u) / 2147483646.0;
        };
        // The generator's first number is small for every small seed, so
        // that seeds 1 to 20 would all start at the same place: skip it.
        static_cast<void>(draw());
        std::vector<Value> sequence;
        if (length == 0)
            return Value::array({});
        const auto places = chain.training.size() - chain.order;
        const auto start = std::min(static_cast<std::size_t>(draw() * static_cast<double>(places)), places - 1);
        std::vector<std::uint32_t> run(chain.training.begin() + static_cast<std::ptrdiff_t>(start),
            chain.training.begin() + static_cast<std::ptrdiff_t>(start + chain.order));
        for (const auto element : run)
        {
            if (sequence.size() < length)
                sequence.push_back(chain.elements[element]);
        }
        while (sequence.size() < length)
        {
            const auto following = chain.following.find(run);
            if (following == chain.following.end())
                break;
            const auto& counts = following->second;
            const auto pick = static_cast<std::size_t>(draw() * static_cast<double>(counts.back().second));
            const auto chosen = std::ranges::upper_bound(
                counts, std::min(pick, counts.back().second - 1), {}, &std::pair<std::uint32_t, std::size_t>::second);
            sequence.push_back(chain.elements[chosen->first]);
            if (!run.empty())
            {
                run.erase(run.begin());
                run.push_back(chosen->first);
            }
        }
        return Value::array(std::move(sequence));
    }

    // The classes a script's values are, each with
    // its superclass, as VisualWorks has them.
    static const std::map<std::string, std::string, std::less<>>& superclasses()
    {
        static const std::map<std::string, std::string, std::less<>> parents{{"SmallInteger", "Integer"},
            {"Integer", "Number"}, {"Double", "Float"}, {"Float", "Number"}, {"Number", "ArithmeticValue"},
            {"Point", "ArithmeticValue"}, {"Complex", "ArithmeticValue"}, {"ArithmeticValue", "Magnitude"},
            {"Date", "Magnitude"}, {"Time", "Magnitude"}, {"Magnitude", "Object"}, {"Symbol", "String"},
            {"String", "ArrayedCollection"}, {"Array", "ArrayedCollection"}, {"ArrayedCollection", "SequenceableCollection"},
            {"OrderedCollection", "SequenceableCollection"}, {"SortedCollection", "OrderedCollection"},
            {"SequenceableCollection", "Collection"}, {"Set", "Collection"}, {"Bag", "Collection"},
            {"Dictionary", "Collection"}, {"Collection", "Object"}, {"True", "Boolean"}, {"False", "Boolean"},
            {"Boolean", "Object"}, {"UndefinedObject", "Object"}, {"BlockClosure", "Object"}, {"Association", "Object"},
            {"Rectangle", "Object"}, {"Filename", "Object"}, {"Random", "Object"}, {"ReadStream", "PositionableStream"},
            {"WriteStream", "PositionableStream"}, {"ReadWriteStream", "WriteStream"}, {"PositionableStream", "Stream"},
            {"Stream", "Object"}, {"SamplesFile", "Object"}, {"MarkovChain", "Object"}, {"OneOverF", "Object"},
            {"EventExpression", "Object"}, {"Class", "Object"}};
        return parents;
    }

    [[nodiscard]] static Value classValue(std::string_view name)
    {
        auto symbol = Value::string(std::string{name});
        symbol.kind = Value::Kind::Symbol;
        return tagged(classTag, {symbol});
    }

    [[nodiscard]] std::string className(const Value& value)
    {
        switch (value.kind)
        {
        case Value::Kind::Nil:
            return "UndefinedObject";
        case Value::Kind::Boolean:
            return value.boolean ? "True" : "False";
        case Value::Kind::Number:
            return value.number == std::floor(value.number) && std::abs(value.number) < 1073741824.0 ? "SmallInteger"
                                                                                                    : "Double";
        case Value::Kind::String:
            return "String";
        case Value::Kind::Symbol:
            return "Symbol";
        case Value::Kind::Expression:
            return "EventExpression";
        case Value::Kind::Block:
            return "BlockClosure";
        case Value::Kind::Array:
            return "Array";
        case Value::Kind::Object:
            break;
        }
        if (partsOf(value, classTag) != nullptr || classNameOf(value).has_value())
            return "Class";
        if (pointOf(value))
            return "Point";
        if (rectangleOf(value))
            return "Rectangle";
        if (complexOf(value))
            return "Complex";
        if (filenameOf(value))
            return "Filename";
        if (dateOrTime(value))
            return isTime(value) ? "Time" : "Date";
        if (partsOf(value, associationTag) != nullptr)
            return "Association";
        if (partsOf(value, imageTag) != nullptr)
            return "Image";
        const auto index = value.index;
        if (index >= firstCollection && index - firstCollection < collections_.size())
        {
            const auto& collection = collections_[index - firstCollection];
            return collection.isSet ? "Set" : collection.bag ? "Bag" : collection.sorted ? "SortedCollection" : "OrderedCollection";
        }
        if (index >= firstDictionary && index - firstDictionary < dictionaries_.size())
            return "Dictionary";
        if (index >= firstReadStream && index - firstReadStream < streams_.size())
            return streams_[index - firstReadStream].writable ? "WriteStream" : "ReadStream";
        if (index >= firstRandom && index - firstRandom < randoms_.size())
            return "Random";
        if (index >= firstOneOverF && index - firstOneOverF < oneOverFs_.size())
            return "OneOverF";
        if (index >= firstMarkovChain && index - firstMarkovChain < markovChains_.size())
            return "MarkovChain";
        if (index >= firstSamplesFile && index - firstSamplesFile < samplesFiles_.size())
            return "SamplesFile";
        return "Object";
    }

    // The name of a class a script holds: a class value, or one of the
    // classes the interpreter names itself, as Random.
    [[nodiscard]] std::optional<std::string> classNameOf(const Value& value)
    {
        if (const auto* named = partsOf(value, classTag))
            return (*named)[0].text;
        if (value.kind != Value::Kind::Object)
            return std::nullopt;
        const std::array<std::pair<std::size_t, std::string_view>, 19> classes{{{pointClass, "Point"},
            {rectangleClass, "Rectangle"}, {randomClass, "Random"}, {floatClass, "Float"}, {dateClass, "Date"},
            {timeClass, "Time"}, {oneOverFClass, "OneOverF"}, {markovChainClass, "MarkovChain"}, {arrayClass, "Array"},
            {readStreamClass, "ReadStream"}, {writeStreamClass, "WriteStream"}, {readWriteStreamClass, "ReadWriteStream"},
            {orderedCollectionClass, "OrderedCollection"}, {setClass, "Set"}, {bagClass, "Bag"},
            {sortedCollectionClass, "SortedCollection"}, {dictionaryClass, "Dictionary"}, {stringClass, "String"},
            {samplesFileClass, "SamplesFile"}}};
        const auto found = std::ranges::find(classes, value.index, &std::pair<std::size_t, std::string_view>::first);
        if (found == classes.end())
            return std::nullopt;
        return std::string{found->second};
    }

    // A class named by Number, Double and the rest: Number readFrom: a
    // stream, and Float or Double errorTolerance.
    Value sendClassValue(const std::string& name, const std::string& selector, std::span<const Value> arguments,
        const SourceRange& range)
    {
        if (selector == "errorTolerance" && arguments.empty() && (name == "Float" || name == "Double"))
        {
            return Value::of(name == "Float" ? static_cast<double>(std::numeric_limits<float>::epsilon())
                                             : std::numeric_limits<double>::epsilon());
        }
        if (selector == "readFrom:" && arguments.size() == 1
            && (name == "Number" || name == "Integer" || name == "SmallInteger" || name == "Double" || name == "Float"))
        {
            const auto character = [&](std::string_view message) {
                const auto answer = send(arguments[0], std::string{message}, {}, range);
                return answer.kind == Value::Kind::String && answer.text.size() == 1 ? answer.text.front() : '\0';
            };
            std::string text;
            while (std::isspace(static_cast<unsigned char>(character("peek"))) != 0)
                (void)send(arguments[0], "next", {}, range);
            for (auto next = character("peek"); next != '\0'; next = character("peek"))
            {
                const auto sign = (next == '-' || next == '+') && (text.empty() || text.back() == 'e');
                if (std::isdigit(static_cast<unsigned char>(next)) == 0 && next != '.' && next != 'e' && !sign)
                    break;
                text += next;
                (void)send(arguments[0], "next", {}, range);
            }
            double number = 0.0;
            const auto [end, error] = talas::fromChars(text.data(), text.data() + text.size(), number);
            if (text.empty() || error != std::errc{} || end != text.data() + text.size())
                throw Failure{"readFrom: finds no number in the stream", range};
            return Value::of(number);
        }
        if (selector == "name" && arguments.empty())
            return Value::string(name);
        throw Failure{name + " does not understand " + selector, range};
    }

    // What printString answers: a number
    // with its unit; a string in quotes, unless displayed; a Symbol bare; an
    // Array's elements each followed by a space within ( ), without the
    // sharp sign, as #({2+5} #(a b c)) prints (7 (a b c ) ); a block as
    // the test window shows one; a Time as 3:05:09 pm and a Date as October
    // 23, 2026, as VisualWorks prints them; a long or deep one cut off with
    // ...etc...
    std::string printed(const Value& value, bool display, std::size_t depth)
    {
        constexpr std::size_t longest = 65'536;
        if (depth > 64)
            return "...etc...";
        const auto parts = [&](std::string text, const std::vector<Value>& elements) {
            text += '(';
            for (const auto& element : elements)
            {
                if (abbreviates_ && text.size() > longest)
                    return text + "...etc... )";
                text += printed(element, false, depth + 1) + ' ';
            }
            return text + ')';
        };
        switch (value.kind)
        {
        case Value::Kind::Nil:
            return "nil";
        case Value::Kind::Boolean:
            return value.boolean ? "true" : "false";
        case Value::Kind::Number:
            if (const auto* exact = exactOf(value))
                return exact->toString();
            // A level in dB is the linear amplitude it stands for, -6 dB
            // printing as 0.501187....
            if (value.unit == Unit::Decibels)
                return numberText(std::pow(10.0, value.number / 20.0));
            return fieldText(value).value_or(std::string{});
        case Value::Kind::Expression:
            return fieldText(value).value_or(std::string{});
        case Value::Kind::String:
        {
            if (display)
                return value.text;
            std::string text = "'";
            for (const auto character : value.text)
            {
                text += character;
                if (character == '\'')
                    text += '\'';
            }
            return text + "'";
        }
        case Value::Kind::Symbol:
            return value.text;
        case Value::Kind::Block:
            return "BlockClosure [] in UndefinedObject>>unboundMethod";
        case Value::Kind::Array:
            return parts("", *value.elements);
        case Value::Kind::Object:
            break;
        }
        if (const auto point = pointOf(value))
            return printed(Value::of(point->first), false, depth + 1) + " @ " + printed(Value::of(point->second), false, depth + 1);
        if (const auto complex = complexOf(value))
        {
            return printed(Value::of(complex->real()), false, depth + 1) + (std::signbit(complex->imag()) ? " - " : " + ")
                + printed(Value::of(std::abs(complex->imag())), false, depth + 1) + " i";
        }
        if (const auto path = filenameOf(value))
            return "a Filename(" + printed(Value::string(*path), false, depth + 1) + ")";
        if (const auto rectangle = rectangleOf(value))
        {
            return printed(newPoint({(*rectangle)[0], (*rectangle)[1]}, {}), false, depth + 1) + " corner: "
                + printed(newPoint({(*rectangle)[2], (*rectangle)[3]}, {}), false, depth + 1);
        }
        if (const auto* named = partsOf(value, classTag))
            return (*named)[0].text;
        if (const auto* association = partsOf(value, associationTag))
            return printed((*association)[0], false, depth + 1) + "->" + printed((*association)[1], false, depth + 1);
        if (dateOrTime(value) && isTime(value))
        {
            const auto seconds = (moment(value) % 86400 + 86400) % 86400;
            const auto hours = seconds / 3600;
            const auto twoDigits = [](std::int64_t number) {
                return (number < 10 ? "0" : "") + std::to_string(number);
            };
            return std::to_string(hours % 12 == 0 ? 12 : hours % 12) + ":" + twoDigits(seconds % 3600 / 60) + ":"
                + twoDigits(seconds % 60) + (hours < 12 ? " am" : " pm");
        }
        if (dateOrTime(value))
        {
            constexpr std::array<std::string_view, 12> months{"January", "February", "March", "April", "May", "June",
                "July", "August", "September", "October", "November", "December"};
            const auto civil = civilDate(moment(value) - daysFrom1901);
            return std::string{months[static_cast<std::size_t>(civil.month - 1)]} + " " + std::to_string(civil.day) + ", "
                + std::to_string(civil.year);
        }
        const auto index = value.index;
        if (index >= firstCollection && index - firstCollection < collections_.size())
        {
            const auto& collection = collections_[index - firstCollection];
            const auto* name = collection.isSet ? "Set " : collection.bag ? "Bag " : collection.sorted ? "SortedCollection "
                                                                                                  : "OrderedCollection ";
            return parts(name, collection.elements);
        }
        if (index >= firstDictionary && index - firstDictionary < dictionaries_.size())
        {
            std::vector<Value> entries;
            for (const auto& [key, held] : dictionaries_[index - firstDictionary].entries)
                entries.push_back(tagged(associationTag, {key, held}));
            return parts("Dictionary ", entries);
        }
        if (index >= firstReadStream && index - firstReadStream < streams_.size())
            return streams_[index - firstReadStream].writable ? "a WriteStream" : "a ReadStream";
        if (index >= firstRandom && index - firstRandom < randoms_.size())
            return "a Random";
        if (index >= firstOneOverF && index - firstOneOverF < oneOverFs_.size())
            return "a OneOverF";
        if (index >= firstMarkovChain && index - firstMarkovChain < markovChains_.size())
            return "a MarkovChain";
        if (index >= firstSamplesFile && index - firstSamplesFile < samplesFiles_.size())
            return "a SamplesFile";
        const std::array<std::pair<std::size_t, std::string_view>, 21> classes{{{pointClass, "Point"},
            {rectangleClass, "Rectangle"}, {hostDriverInterfaceClass, "HostDriverInterface"},
            {randomClass, "Random"},
            {floatClass, "Float"}, {dateClass, "Date"}, {timeClass, "Time"}, {oneOverFClass, "OneOverF"},
            {markovChainClass, "MarkovChain"}, {arrayClass, "Array"}, {readStreamClass, "ReadStream"},
            {writeStreamClass, "WriteStream"}, {readWriteStreamClass, "ReadWriteStream"},
            {orderedCollectionClass, "OrderedCollection"}, {setClass, "Set"}, {bagClass, "Bag"},
            {sortedCollectionClass, "SortedCollection"}, {dictionaryClass, "Dictionary"}, {stringClass, "String"},
            {samplesFileClass, "SamplesFile"}, {bindingListClass, "BindingList"}}};
        if (const auto found = std::ranges::find(classes, index, &std::pair<std::size_t, std::string_view>::first);
            found != classes.end())
            return std::string{found->second};
        // The host's own objects print as it says.
        std::string error;
        if (const auto answer = host_.send(value, "printString", {}, error);
            answer.has_value() && answer->kind == Value::Kind::String)
            return answer->text;
        return "an Object";
    }

    // A key two elements share when = finds them alike.
    [[nodiscard]] std::string elementKey(const Value& value)
    {
        std::string key(1, static_cast<char>('a' + static_cast<int>(value.kind)));
        switch (value.kind)
        {
        case Value::Kind::Nil:
            break;
        case Value::Kind::Boolean:
            key += value.boolean ? '1' : '0';
            break;
        case Value::Kind::Number:
        {
            const auto kind = quantity(value.unit);
            key += std::to_string(static_cast<int>(kind)) + ':';
            if (kind == Quantity::Other)
                key += std::to_string(static_cast<int>(value.unit)) + ':';
            const auto base = toBase(value.number, value.unit);
            // Zero alike in either sign; a NaN like nothing.
            key += std::isnan(base) ? "nan" + std::to_string(unlikeKeys_++)
                                    : std::to_string(std::bit_cast<std::uint64_t>(base == 0.0 ? 0.0 : base));
            break;
        }
        case Value::Kind::String:
        case Value::Kind::Symbol:
        case Value::Kind::Expression:
            key += value.text;
            break;
        case Value::Kind::Object:
        case Value::Kind::Block:
            key += std::to_string(value.index);
            // A Point, Date or other value of its parts is alike by them.
            if (value.kind == Value::Kind::Object && value.elements != nullptr)
            {
                for (const auto& element : *value.elements)
                {
                    const auto part = elementKey(element);
                    key += std::to_string(part.size()) + ':' + part;
                }
            }
            break;
        case Value::Kind::Array:
            for (const auto& element : *value.elements)
            {
                const auto part = elementKey(element);
                key += std::to_string(part.size()) + ':' + part;
            }
            break;
        }
        return key;
    }

    // Array with: a with: b (up to six), Array new: n (of nils) and Array
    // new: n withAll: value.
    Value sendArrayClass(const std::string& selector, std::span<const Value> arguments, const SourceRange& range)
    {
        auto keywords = std::string_view{selector};
        while (keywords.starts_with("with:"))
            keywords.remove_prefix(std::string_view{"with:"}.size());
        if (keywords.empty() && !arguments.empty())
            return Value::array({arguments.begin(), arguments.end()});
        if (selector == "new" && arguments.empty())
            return Value::array({});
        if ((selector == "new:" || selector == "new:withAll:") && arguments[0].kind == Value::Kind::Number)
        {
            const auto count = arguments[0].number;
            if (count < 0.0 || count != std::floor(count) || count > static_cast<double>(maximumElements))
                throw Failure{"An Array's size is a whole number within the fixed element limit", range};
            return Value::array(std::vector<Value>(static_cast<std::size_t>(count),
                selector == "new:" ? Value::nil() : arguments[1]));
        }
        throw Failure{"Array understands with:, new and new:", range};
    }

    // The messages to an Array:
    // size and the elements, iteration, and new Arrays made from it.
    Value sendArray(const Value& array, const std::string& selector, std::span<const Value> arguments,
        const SourceRange& range)
    {
        const auto& elements = *array.elements;
        const auto position = [&](const Value& index) {
            if (index.kind != Value::Kind::Number || index.number != std::floor(index.number) || index.number < 1.0
                || index.number > static_cast<double>(elements.size()))
            {
                throw Failure{"An index lies outside the Array", range};
            }
            return static_cast<std::size_t>(index.number) - 1;
        };
        if (selector == "size")
            return Value::of(static_cast<double>(elements.size()));
        if (selector == "isEmpty" || selector == "notEmpty")
            return Value::of(elements.empty() == (selector == "isEmpty"));
        if (selector == "first" || selector == "last")
        {
            if (elements.empty())
                throw Failure{"An empty Array has no " + selector, range};
            return selector == "first" ? elements.front() : elements.back();
        }
        if (selector == "at:")
            return elements[position(arguments[0])];
        if (selector == "asArray" || selector == "copy")
            return array;
        if (selector == "reverse" || selector == "reversed")
            return Value::array({elements.rbegin(), elements.rend()});
        if (selector == "," && arguments[0].kind == Value::Kind::Array)
        {
            auto joined = elements;
            joined.insert(joined.end(), arguments[0].elements->begin(), arguments[0].elements->end());
            return Value::array(std::move(joined));
        }
        if (selector == "copyFrom:to:")
        {
            const auto from = position(arguments[0]);
            const auto to = position(arguments[1]);
            if (to < from)
                return Value::array({});
            return Value::array({elements.begin() + static_cast<std::ptrdiff_t>(from),
                elements.begin() + static_cast<std::ptrdiff_t>(to) + 1});
        }
        if (selector == "includes:")
        {
            return Value::of(std::ranges::any_of(elements, [&](const Value& element) {
                return element.kind == arguments[0].kind && equal(element, arguments[0], range);
            }));
        }
        if (selector == "do:")
        {
            for (const auto& element : elements)
                callBlock(arguments[0], std::array{element}, range);
            return array;
        }
        if (selector == "doWithIndex:" || selector == "keysAndValuesDo:")
        {
            for (std::size_t index = 0; index < elements.size(); ++index)
            {
                const auto one = Value::of(static_cast<double>(index + 1));
                if (selector == "doWithIndex:")
                    callBlock(arguments[0], std::array{elements[index], one}, range);
                else
                    callBlock(arguments[0], std::array{one, elements[index]}, range);
            }
            return array;
        }
        if (selector == "collect:")
        {
            std::vector<Value> collected;
            collected.reserve(elements.size());
            for (const auto& element : elements)
                collected.push_back(callBlock(arguments[0], std::array{element}, range));
            return Value::array(std::move(collected));
        }
        if (selector == "with:collect:")
        {
            if (arguments[0].kind != Value::Kind::Array || arguments[0].elements->size() != elements.size())
                throw Failure{"with:collect: needs an Array of the same size", range};
            std::vector<Value> collected;
            for (std::size_t index = 0; index < elements.size(); ++index)
                collected.push_back(callBlock(arguments[1], std::array{elements[index], (*arguments[0].elements)[index]}, range));
            return Value::array(std::move(collected));
        }
        if (selector == "select:" || selector == "reject:")
        {
            std::vector<Value> kept;
            for (const auto& element : elements)
            {
                if (truth(callBlock(arguments[0], std::array{element}, range), range) == (selector == "select:"))
                    kept.push_back(element);
            }
            return Value::array(std::move(kept));
        }
        if (selector == "detect:" || selector == "detect:ifNone:")
        {
            for (const auto& element : elements)
            {
                if (truth(callBlock(arguments[0], std::array{element}, range), range))
                    return element;
            }
            if (selector == "detect:ifNone:")
                return callBlock(arguments[1], {}, range);
            throw Failure{"No element was detected", range};
        }
        if (selector == "inject:into:")
        {
            auto total = arguments[0];
            for (const auto& element : elements)
                total = callBlock(arguments[1], std::array{total, element}, range);
            return total;
        }
        if (selector == "max" || selector == "min" || selector == "sum")
        {
            if (elements.empty())
                throw Failure{"An empty Array has no " + selector, range};
            auto result = elements.front();
            for (std::size_t index = 1; index < elements.size(); ++index)
            {
                const auto& element = elements[index];
                if (selector == "sum")
                    result = send(result, "+", std::array{element}, range);
                else if (truth(send(element, selector == "max" ? ">" : "<", std::array{result}, range), range))
                    result = element;
            }
            return result;
        }
        const auto same = [&](const Value& element, const Value& other) {
            return element.kind == other.kind && equal(element, other, range);
        };
        if (selector == "indexOf:" || selector == "lastIndexOf:" || selector == "identityIndexOf:")
        {
            for (std::size_t step = 0; step < elements.size(); ++step)
            {
                const auto index = selector == "lastIndexOf:" ? elements.size() - 1 - step : step;
                if (same(elements[index], arguments[0]))
                    return Value::of(static_cast<double>(index + 1));
            }
            return Value::of(0.0);
        }
        // The next or previous index of an element between
        // two indices, nil when there is none, and an element chosen by a
        // Random's next value.
        if (selector == "nextIndexOf:from:to:" || selector == "prevIndexOf:from:to:")
        {
            const auto bound = [&](const Value& index) {
                if (index.kind != Value::Kind::Number || index.number != std::floor(index.number))
                    throw Failure{selector + " takes whole indices", range};
                return static_cast<std::int64_t>(index.number);
            };
            const auto start = bound(arguments[1]);
            const auto stop = bound(arguments[2]);
            const auto next = selector == "nextIndexOf:from:to:";
            for (auto index = next ? start : stop; next ? index <= stop : index >= start; index += next ? 1 : -1)
            {
                if (index >= 1 && index <= static_cast<std::int64_t>(elements.size())
                    && same(elements[static_cast<std::size_t>(index - 1)], arguments[0]))
                {
                    return Value::of(static_cast<double>(index));
                }
            }
            return Value::nil();
        }
        if (selector == "selectRandomElementUsing:")
        {
            if (elements.empty())
                throw Failure{"An empty collection has no element to choose", range};
            const auto drawn = numberOf(send(arguments[0], "next", {}, range));
            if (drawn.kind != Value::Kind::Number)
                throw Failure{"selectRandomElementUsing: takes a Random", range};
            const auto index = std::min(elements.size() - 1,
                static_cast<std::size_t>(std::max(0.0, std::floor(drawn.number * static_cast<double>(elements.size())))));
            return elements[index];
        }
        // Iterating backwards and by index, finding
        // by a block or a run of elements, and copies with an element added,
        // removed or a run replaced.
        if (selector == "reverseDo:")
        {
            for (auto element = elements.rbegin(); element != elements.rend(); ++element)
                callBlock(arguments[0], std::array{*element}, range);
            return array;
        }
        if (selector == "keysDo:")
        {
            for (std::size_t index = 0; index < elements.size(); ++index)
                callBlock(arguments[0], std::array{Value::of(static_cast<double>(index + 1))}, range);
            return array;
        }
        if (selector == "findFirst:" || selector == "findLast:")
        {
            for (std::size_t step = 0; step < elements.size(); ++step)
            {
                const auto index = selector == "findFirst:" ? step : elements.size() - 1 - step;
                if (truth(callBlock(arguments[0], std::array{elements[index]}, range), range))
                    return Value::of(static_cast<double>(index + 1));
            }
            return Value::of(0.0);
        }
        const auto run = [&](const Value& value) -> const std::vector<Value>& {
            if (value.kind != Value::Kind::Array)
                throw Failure{selector + " takes an Array", range};
            return *value.elements;
        };
        const auto at = [&](const std::vector<Value>& part, std::size_t from) {
            return !part.empty() && from + part.size() <= elements.size()
                && std::equal(part.begin(), part.end(), elements.begin() + static_cast<std::ptrdiff_t>(from), same);
        };
        if (selector == "indexOfSubCollection:startingAt:")
        {
            const auto& part = run(arguments[0]);
            const auto start = position(arguments[1]);
            for (auto from = start; from < elements.size(); ++from)
            {
                if (at(part, from))
                    return Value::of(static_cast<double>(from + 1));
            }
            return Value::of(0.0);
        }
        if (selector == "copyWith:")
        {
            if (elements.size() >= maximumElements)
                throw Failure{"An Array holds at most 65536 elements", range};
            auto copy = elements;
            copy.push_back(arguments[0]);
            return Value::array(std::move(copy));
        }
        if (selector == "copyWithout:")
        {
            std::vector<Value> copy;
            for (const auto& element : elements)
            {
                if (!same(element, arguments[0]))
                    copy.push_back(element);
            }
            return Value::array(std::move(copy));
        }
        if (selector == "copyReplaceAll:with:")
        {
            const auto& old = run(arguments[0]);
            const auto& replacement = run(arguments[1]);
            std::vector<Value> copy;
            for (std::size_t from = 0; from < elements.size();)
            {
                if (at(old, from))
                {
                    copy.insert(copy.end(), replacement.begin(), replacement.end());
                    from += old.size();
                }
                else
                    copy.push_back(elements[from++]);
                if (copy.size() > maximumElements)
                    throw Failure{"An Array holds at most 65536 elements", range};
            }
            return Value::array(std::move(copy));
        }
        if (selector == "copyReplaceFrom:to:with:")
        {
            // From start to stop replaced; stop one before start inserts.
            const auto& replacement = run(arguments[2]);
            const auto bound = [&](const Value& index) {
                if (index.kind != Value::Kind::Number || index.number != std::floor(index.number) || index.number < 0.0
                    || index.number > static_cast<double>(elements.size()) + 1.0)
                {
                    throw Failure{"copyReplaceFrom:to:with: takes positions within the Array", range};
                }
                return static_cast<std::size_t>(index.number);
            };
            const auto start = bound(arguments[0]);
            const auto stop = bound(arguments[1]);
            if (start < 1 || stop + 1 < start || stop > elements.size())
                throw Failure{"copyReplaceFrom:to:with: takes positions within the Array", range};
            std::vector<Value> copy(elements.begin(), elements.begin() + static_cast<std::ptrdiff_t>(start - 1));
            copy.insert(copy.end(), replacement.begin(), replacement.end());
            copy.insert(copy.end(), elements.begin() + static_cast<std::ptrdiff_t>(stop), elements.end());
            if (copy.size() > maximumElements)
                throw Failure{"An Array holds at most 65536 elements", range};
            return Value::array(std::move(copy));
        }
        if (selector == "asBag" || selector == "asSet" || selector == "asOrderedCollection"
            || selector == "asSortedCollection" || selector == "asSortedCollection:" || selector == "readStream"
            || selector == "writeStream")
            return sendElements(elements, selector, arguments, range);
        throw Failure{"An Array does not understand " + selector, range};
    }

    // self debug: value shows the value, and self debugWithLabel: label
    // value: value the label and the value, each answering the value, so
    // a script can inspect its variables as it runs.
    // The elements of an Array, or the characters of a string as strings of
    // one.
    std::vector<Value> elementsOf(const Value& value, const SourceRange& range) const
    {
        if (value.kind == Value::Kind::Array)
            return *value.elements;
        if (value.kind == Value::Kind::String)
        {
            std::vector<Value> characters;
            for (const auto character : value.text)
                characters.push_back(Value::string(std::string(1, character)));
            return characters;
        }
        if (value.kind == Value::Kind::Object && value.index >= firstCollection
            && value.index - firstCollection < collections_.size())
            return collections_[value.index - firstCollection].elements;
        if (value.kind == Value::Kind::Object && value.index >= firstDictionary
            && value.index - firstDictionary < dictionaries_.size())
        {
            std::vector<Value> values;
            for (const auto& entry : dictionaries_[value.index - firstDictionary].entries)
                values.push_back(entry.second);
            return values;
        }
        throw Failure{"Expected an Array, a string or a collection", range};
    }

    // ReadStream on: a collection: next answers the next
    // element and advances past it, nil at the end; atEnd tells whether
    // all have been read.
    Value sendReadStream(const Value& receiver, const std::string& selector, std::span<const Value> arguments,
        const SourceRange& range)
    {
        if (receiver.index == readStreamClass || receiver.index == writeStreamClass
            || receiver.index == readWriteStreamClass)
        {
            if (selector != "on:")
                throw Failure{"A stream class understands on:", range};
            return makeStream(elementsOf(arguments[0], range),
                arguments[0].kind == Value::Kind::String || arguments[0].kind == Value::Kind::Symbol,
                receiver.index == writeStreamClass, range);
        }
        auto& stream = streams_[receiver.index - firstReadStream];
        if (stream.file.has_value())
        {
            if (auto answer = sendFileStream(stream, receiver, selector, range))
                return *answer;
        }
        const auto atEnd = stream.position >= stream.elements.size();
        // A run of the stream's elements, a string's as a string.
        const auto part = [&](std::size_t from, std::size_t to) {
            std::vector<Value> run(stream.elements.begin() + static_cast<std::ptrdiff_t>(from),
                stream.elements.begin() + static_cast<std::ptrdiff_t>(to));
            if (!stream.text)
                return Value::array(std::move(run));
            std::string joined;
            for (const auto& element : run)
                joined += element.text;
            return Value::string(std::move(joined));
        };
        const auto same = [&](const Value& element, const Value& other) {
            return element.kind == other.kind && equal(element, other, range);
        };
        const auto search = [&](const Value& wanted, bool many) {
            const auto run = many ? elementsOf(wanted, range) : std::vector<Value>{wanted};
            for (auto from = stream.position; run.size() > 0 && from + run.size() <= stream.elements.size(); ++from)
            {
                if (std::equal(run.begin(), run.end(), stream.elements.begin() + static_cast<std::ptrdiff_t>(from), same))
                    return std::pair{from, run.size()};
            }
            return std::pair{stream.elements.size(), std::size_t{0}};
        };
        const auto write = [&](const Value& element) {
            stream.dirty = stream.file.has_value();
            // An append stream writes at the end, reading on from where it
            // read.
            if (stream.appending)
            {
                if (stream.elements.size() >= maximumElements)
                    throw Failure{"A stream holds at most 65536 elements", range};
                stream.elements.push_back(element);
                if (!stream.reads)
                    stream.position = stream.elements.size();
                return;
            }
            if (stream.position >= maximumElements)
                throw Failure{"A stream holds at most 65536 elements", range};
            stream.elements.resize(stream.position);
            stream.elements.push_back(element);
            ++stream.position;
        };
        // The elements read and written as bytes, the
        // most significant first unless LE puts the least first, signed or
        // not, and strings ending in a 0 or led by their length.
        const auto byteAt = [&](std::size_t index) {
            const auto& element = stream.elements[index];
            const auto code = element.kind == Value::Kind::String && element.text.size() == 1
                ? static_cast<double>(static_cast<unsigned char>(element.text.front()))
                : element.kind == Value::Kind::Number ? element.number : -1.0;
            if (code < 0.0 || code > 255.0 || code != std::floor(code))
                throw Failure{"A stream read as bytes holds numbers from 0 to 255", range};
            return static_cast<std::uint64_t>(code);
        };
        struct BinaryReader
        {
            std::string_view selector;
            std::size_t bytes;
            bool little;
            bool isSigned;
        };
        constexpr std::array<BinaryReader, 12> readers{{
            {"nextSigned8Bit", 1, false, true}, {"nextWord", 2, false, false}, {"nextWordLE", 2, true, false},
            {"nextSigned16Bit", 2, false, true}, {"nextSigned16BitLE", 2, true, true}, {"next24Bit", 3, false, false},
            {"next24BitLE", 3, true, false}, {"nextSigned24Bit", 3, false, true}, {"nextSigned24BitLE", 3, true, true},
            {"next32Bit", 4, false, false}, {"nextLong", 4, false, true}, {"nextLongLE", 4, true, true},
        }};
        const auto reader = std::ranges::find(readers, std::string_view{selector}, &BinaryReader::selector);
        const auto readBytes = [&](std::size_t bytes, bool little) {
            if (stream.position + bytes > stream.elements.size())
                throw Failure{"The stream has too few bytes for " + selector, range};
            std::uint64_t value = 0;
            for (std::size_t index = 0; index < bytes; ++index)
            {
                const auto byte = byteAt(stream.position + index);
                value = little ? value | (byte << (8 * index)) : (value << 8) | byte;
            }
            stream.position += bytes;
            return value;
        };
        if (reader != readers.end() && arguments.empty())
        {
            const auto value = readBytes(reader->bytes, reader->little);
            const auto top = std::uint64_t{1} << (8 * reader->bytes - 1);
            return Value::of(reader->isSigned && (value & top) != 0
                    ? static_cast<double>(value) - static_cast<double>(top) * 2.0
                    : static_cast<double>(value));
        }
        if ((selector == "nextCString" || selector == "nextString") && arguments.empty())
        {
            std::string read;
            if (selector == "nextCString")
            {
                for (auto byte = readBytes(1, false); byte != 0; byte = readBytes(1, false))
                    read += static_cast<char>(byte);
            }
            else
            {
                auto length = readBytes(1, false);
                if (length > 192)
                    length = ((length - 192) << 8) | readBytes(1, false);
                for (std::uint64_t index = 0; index < length; ++index)
                    read += static_cast<char>(readBytes(1, false));
            }
            return Value::string(std::move(read));
        }
        // An 80-bit extended float, as an AIFF header's
        // sample rate, and the 96-bit form with 16 bits of padding after
        // the exponent: sign, 15-bit exponent, 64-bit mantissa with its
        // integer bit.
        if ((selector == "next80BitFloat" || selector == "next96BitFloat" || selector == "next96BitDouble") && arguments.empty())
        {
            if (stream.position + (selector == "next80BitFloat" ? 10 : 12) > stream.elements.size())
                throw Failure{"The stream has too few bytes for " + selector, range};
            const auto signAndExponent = readBytes(2, false);
            if (selector != "next80BitFloat")
                (void)readBytes(2, false);
            const auto mantissa = readBytes(8, false);
            const auto exponent = static_cast<int>(signAndExponent & 0x7fffU);
            const auto sign = (signAndExponent & 0x8000U) != 0 ? -1.0 : 1.0;
            if (exponent == 0x7fff)
            {
                if ((mantissa << 1U) != 0)
                    throw Failure{"The stream's extended float is not a number", range};
                return Value::of(sign * std::numeric_limits<double>::infinity());
            }
            return Value::of(sign * std::ldexp(static_cast<double>(mantissa), exponent - 16383 - 63));
        }
        const auto writeBytes = [&](std::uint64_t value, std::size_t bytes, bool little) {
            for (std::size_t index = 0; index < bytes; ++index)
            {
                const auto shift = little ? 8 * index : 8 * (bytes - 1 - index);
                write(Value::of(static_cast<double>((value >> shift) & 0xffu)));
            }
        };
        struct BinaryWriter
        {
            std::string_view selector;
            std::size_t bytes;
            bool little;
            bool isSigned;
        };
        constexpr std::array<BinaryWriter, 6> writers{{
            {"nextWordPut:", 2, false, false}, {"nextWordLEPut:", 2, true, false}, {"next24BitPut:", 3, false, false},
            {"next32BitPut:", 4, false, false}, {"nextLongPut:", 4, false, true}, {"nextLongLEPut:", 4, true, true},
        }};
        const auto writer = std::ranges::find(writers, std::string_view{selector}, &BinaryWriter::selector);
        if (writer != writers.end() && arguments.size() == 1)
        {
            const auto number = numberOf(arguments[0]);
            const auto span = std::ldexp(1.0, static_cast<int>(8 * writer->bytes));
            if (number.kind != Value::Kind::Number || number.number != std::floor(number.number)
                || number.number < (writer->isSigned ? -span / 2.0 : 0.0)
                || number.number >= (writer->isSigned ? span / 2.0 : span))
                throw Failure{selector + " takes a whole number that fits its bytes", range};
            const auto value = number.number < 0.0 ? number.number + span : number.number;
            writeBytes(static_cast<std::uint64_t>(value), writer->bytes, writer->little);
            return arguments[0];
        }
        if ((selector == "nextCStringPut:" || selector == "nextStringPut:") && arguments.size() == 1)
        {
            if (arguments[0].kind != Value::Kind::String && arguments[0].kind != Value::Kind::Symbol)
                throw Failure{selector + " takes a String", range};
            const auto& written = arguments[0].text;
            if (selector == "nextStringPut:")
            {
                if (written.size() > 16383)
                    throw Failure{"nextStringPut: writes at most 16383 characters", range};
                if (written.size() <= 192)
                    writeBytes(written.size(), 1, false);
                else
                    writeBytes(((written.size() >> 8) + 192) << 8 | (written.size() & 0xffu), 2, false);
            }
            for (const auto character : written)
                writeBytes(static_cast<unsigned char>(character), 1, false);
            if (selector == "nextCStringPut:")
                writeBytes(0, 1, false);
            return arguments[0];
        }
        if (selector == "padTo:" && arguments.size() == 1)
        {
            const auto boundary = numberOf(arguments[0]);
            if (boundary.kind != Value::Kind::Number || boundary.number < 1.0 || boundary.number != std::floor(boundary.number)
                || boundary.number > 65536.0)
                throw Failure{"padTo: takes a whole number of bytes", range};
            const auto size = static_cast<std::size_t>(boundary.number);
            const auto skipped = (size - stream.position % size) % size;
            for (std::size_t index = 0; index < skipped; ++index)
                write(Value::of(0.0));
            return Value::of(static_cast<double>(skipped));
        }
        // Reading runs of a stream, up to or through
        // an element or a run of them, skipping, and its position; writing
        // elements, printed values and white space.
        if (selector == "next:" || selector == "nextAvailable:")
        {
            if (arguments[0].kind != Value::Kind::Number || arguments[0].number < 0.0)
                throw Failure{selector + " takes a count", range};
            const auto wanted = static_cast<std::size_t>(arguments[0].number);
            const auto left = stream.elements.size() - std::min(stream.position, stream.elements.size());
            if (selector == "next:" && wanted > left)
                throw Failure{"The stream has fewer elements left than next: asks for", range};
            const auto from = stream.position;
            stream.position += std::min(wanted, left);
            return part(from, stream.position);
        }
        if (selector == "through:" || selector == "throughAll:" || selector == "upTo:" || selector == "upToAll:")
        {
            const auto [found, length] = search(arguments[0], selector.ends_with("All:"));
            const auto from = stream.position;
            const auto inclusive = selector.starts_with("through");
            const auto end = found + (length > 0 && inclusive ? length : 0);
            stream.position = found + length;
            return part(from, std::min(end, stream.elements.size()));
        }
        if (selector == "skipThrough:" || selector == "skipUpTo:" || selector == "skipToAll:")
        {
            const auto [found, length] = search(arguments[0], selector == "skipToAll:");
            if (length == 0)
            {
                stream.position = stream.elements.size();
                return Value::nil();
            }
            stream.position = selector == "skipThrough:" ? found + length : found;
            return receiver;
        }
        if (selector == "nextMatchFor:")
            return Value::of(!atEnd && same(stream.elements[stream.position++], arguments[0]));
        if (selector == "position")
            return Value::of(static_cast<double>(stream.position));
        if (selector == "position:")
        {
            if (arguments[0].kind != Value::Kind::Number || arguments[0].number < 0.0
                || arguments[0].number > static_cast<double>(stream.elements.size()))
                throw Failure{"position: lies within the stream", range};
            stream.position = static_cast<std::size_t>(arguments[0].number);
            return receiver;
        }
        if (selector == "contentsBetween:and:")
        {
            const auto bound = [&](const Value& index) {
                if (index.kind != Value::Kind::Number || index.number < 1.0
                    || index.number > static_cast<double>(stream.elements.size()))
                    throw Failure{"contentsBetween:and: takes positions within the stream", range};
                return static_cast<std::size_t>(index.number);
            };
            const auto from = bound(arguments[0]);
            const auto to = bound(arguments[1]);
            return to < from ? part(0, 0) : part(from - 1, to);
        }
        if (selector == "close" || selector == "flush")
            return receiver;
        if (selector == "nextPut:")
        {
            write(arguments[0]);
            return arguments[0];
        }
        if (selector == "nextPutAll:" || selector == "print:" || selector == "cr" || selector == "space"
            || selector == "tab" || selector == "next:put:")
        {
            std::vector<Value> written;
            if (selector == "nextPutAll:")
                written = elementsOf(arguments[0], range);
            else if (selector == "next:put:")
            {
                if (arguments[0].kind != Value::Kind::Number || arguments[0].number < 0.0
                    || arguments[0].number > static_cast<double>(maximumElements))
                    throw Failure{"next:put: takes a count", range};
                written.assign(static_cast<std::size_t>(arguments[0].number), arguments[1]);
            }
            else
            {
                const auto printed = selector == "cr" ? std::optional<std::string>{stream.lineEnd}
                    : selector == "space"            ? std::optional<std::string>{" "}
                    : selector == "tab"              ? std::optional<std::string>{"\t"}
                    : arguments[0].kind == Value::Kind::String || arguments[0].kind == Value::Kind::Symbol
                        ? std::optional<std::string>{arguments[0].text}
                        : fieldText(arguments[0]);
                if (!printed.has_value())
                    throw Failure{"print: writes a printable value", range};
                if (stream.text)
                {
                    for (const auto character : *printed)
                        written.push_back(Value::string(std::string(1, character)));
                }
                else
                    written.push_back(Value::string(*printed));
            }
            for (const auto& element : written)
                write(element);
            return receiver;
        }
        if (selector == "atEnd")
            return Value::of(atEnd);
        if (selector == "next")
            return atEnd ? Value::nil() : stream.elements[stream.position++];
        if (selector == "peek")
            return atEnd ? Value::nil() : stream.elements[stream.position];
        if (selector == "skip:" && arguments[0].kind == Value::Kind::Number)
        {
            const auto moved = static_cast<double>(stream.position) + std::floor(arguments[0].number);
            stream.position = static_cast<std::size_t>(
                std::clamp(moved, 0.0, static_cast<double>(stream.elements.size())));
            return receiver;
        }
        if (selector == "upToEnd")
        {
            const auto from = std::min(stream.position, stream.elements.size());
            stream.position = stream.elements.size();
            return part(from, stream.elements.size());
        }
        if (selector == "reset")
        {
            stream.position = 0;
            return receiver;
        }
        // Everything the stream reads, a string's characters as a string
        //.
        if (selector == "contents")
            return part(0, stream.elements.size());
        // A text's next line as an Array of its numbers and words, the
        // words Symbols, as a CSound score's i 1 0 2 8.09.
        if (selector == "nextParameters" && stream.text)
        {
            std::string line;
            while (stream.position < stream.elements.size())
            {
                const auto& character = stream.elements[stream.position++].text;
                if (character == "\n")
                    break;
                if (character == "\r")
                {
                    if (stream.position < stream.elements.size() && stream.elements[stream.position].text == "\n")
                        ++stream.position;
                    break;
                }
                line += character;
            }
            std::vector<Value> items;
            std::size_t at = 0;
            while ((at = line.find_first_not_of(" \t", at)) != std::string::npos)
            {
                const auto end = std::min(line.find_first_of(" \t", at), line.size());
                const auto word = line.substr(at, end - at);
                at = end;
                char* parsed = nullptr;
                const auto number = std::strtod(word.c_str(), &parsed);
                if (parsed == word.c_str() + word.size() && std::isfinite(number))
                {
                    items.push_back(Value::of(number));
                    continue;
                }
                auto symbol = Value::string(word);
                symbol.kind = Value::Kind::Symbol;
                items.push_back(std::move(symbol));
            }
            return Value::array(std::move(items));
        }
        throw Failure{"A stream does not understand " + selector, range};
    }

    // OrderedCollection and Set: collections a script
    // changes in place. removeFirst answers the first element and removes
    // it; a Set holds each element once.
    Value sendCollection(const Value& receiver, const std::string& selector, std::span<const Value> arguments,
        const SourceRange& range)
    {
        if (receiver.index == orderedCollectionClass || receiver.index == setClass || receiver.index == bagClass
            || receiver.index == sortedCollectionClass)
        {
            Collection made;
            made.isSet = receiver.index == setClass;
            made.bag = receiver.index == bagClass;
            made.sorted = receiver.index == sortedCollectionClass;
            if (selector == "sortBlock:" && made.sorted)
                made.sortBlock = arguments[0];
            else if (selector == "withAll:")
                made.elements = elementsOf(arguments[0], range);
            else if (selector != "new" && selector != "new:")
                throw Failure{"A collection class understands new, new:, withAll: and sortBlock:", range};
            return makeCollection(std::move(made), range);
        }
        auto& collection = collections_[receiver.index - firstCollection];
        auto& elements = collection.elements;
        if (selector == "add:" && collection.sorted)
        {
            if (elements.size() >= maximumElements)
                throw Failure{"A collection holds at most 65536 elements", range};
            const auto place = std::ranges::find_if(elements, [&](const Value& held) {
                return !before(collection, held, arguments[0], range);
            });
            elements.insert(place, arguments[0]);
            return arguments[0];
        }
        // A Bag's element added many times, whether it
        // holds one twice, and a Set's difference, any element and overlap.
        if (selector == "add:withOccurrences:")
        {
            if (arguments[1].kind != Value::Kind::Number || arguments[1].number < 0.0
                || arguments[1].number > static_cast<double>(maximumElements))
                throw Failure{"add:withOccurrences: takes a count", range};
            for (double count = 0.0; count < arguments[1].number; count += 1.0)
                add(collection.isSet, elements, arguments[0], range);
            return arguments[0];
        }
        if (selector == "containsDuplicates")
        {
            for (std::size_t index = 0; index < elements.size(); ++index)
            {
                for (auto other = index + 1; other < elements.size(); ++other)
                {
                    if (equal(elements[index], elements[other], range))
                        return Value::of(true);
                }
            }
            return Value::of(false);
        }
        if (selector == "-" && collection.isSet)
        {
            const auto removed = elementsOf(arguments[0], range);
            Collection difference;
            difference.isSet = true;
            for (const auto& element : elements)
            {
                if (std::ranges::none_of(removed, [&](const Value& other) { return equal(element, other, range); }))
                    difference.elements.push_back(element);
            }
            return makeCollection(std::move(difference), range);
        }
        if (selector == "anyElement")
            return elements.empty() ? Value::nil() : elements.front();
        if (selector == "intersects:")
        {
            const auto others = elementsOf(arguments[0], range);
            return Value::of(std::ranges::any_of(elements, [&](const Value& element) {
                return std::ranges::any_of(others, [&](const Value& other) { return equal(element, other, range); });
            }));
        }
        // An OrderedCollection's elements by position from 1.
        if ((selector == "at:" || selector == "at:put:" || selector == "swap:with:") && !collection.isSet && !collection.bag)
        {
            const auto position = [&](const Value& index) {
                if (index.kind != Value::Kind::Number || index.number != std::floor(index.number) || index.number < 1.0
                    || index.number > static_cast<double>(elements.size()))
                    throw Failure{"An index lies outside the collection", range};
                return static_cast<std::size_t>(index.number) - 1;
            };
            if (selector == "at:")
                return elements[position(arguments[0])];
            if (selector == "swap:with:")
            {
                std::swap(elements[position(arguments[0])], elements[position(arguments[1])]);
                return receiver;
            }
            if (collection.sorted)
                throw Failure{"A SortedCollection keeps its own order", range};
            elements[position(arguments[0])] = arguments[1];
            return arguments[1];
        }
        if (selector == "atAllPut:" && !collection.isSet && !collection.bag && !collection.sorted)
        {
            std::ranges::fill(elements, arguments[0]);
            return receiver;
        }
        const auto empty = [&] {
            if (elements.empty())
                throw Failure{"An empty collection has no element to " + selector, range};
        };
        if (selector == "add:" || selector == "addLast:")
        {
            add(collection.isSet, elements, arguments[0], range);
            return arguments[0];
        }
        // Adding next to an element or a position, and
        // removing at a position, in an OrderedCollection; where a
        // SortedCollection would put an element.
        if ((selector == "add:after:" || selector == "add:before:" || selector == "add:afterIndex:"
                || selector == "add:beforeIndex:")
            && !collection.isSet && !collection.bag && !collection.sorted)
        {
            std::size_t spot = 0;
            if (selector == "add:after:" || selector == "add:before:")
            {
                const auto found = std::ranges::find_if(elements, [&](const Value& held) { return equal(held, arguments[1], range); });
                if (found == elements.end())
                    throw Failure{"The collection does not hold the element to add next to", range};
                spot = static_cast<std::size_t>(found - elements.begin()) + (selector == "add:after:" ? 1 : 0);
            }
            else
            {
                const auto index = numberOf(arguments[1]);
                const auto after = selector == "add:afterIndex:";
                if (index.kind != Value::Kind::Number || index.number != std::floor(index.number)
                    || index.number < (after ? 0.0 : 1.0)
                    || index.number > static_cast<double>(elements.size()) + (after ? 0.0 : 1.0))
                    throw Failure{"An index lies outside the collection", range};
                spot = static_cast<std::size_t>(index.number) - (after ? 0 : 1);
            }
            if (elements.size() >= maximumElements)
                throw Failure{"A collection holds at most 65536 elements", range};
            elements.insert(elements.begin() + static_cast<std::ptrdiff_t>(spot), arguments[0]);
            return arguments[0];
        }
        if (selector == "removeAtIndex:" && !collection.isSet && !collection.bag)
        {
            const auto index = numberOf(arguments[0]);
            if (index.kind != Value::Kind::Number || index.number != std::floor(index.number) || index.number < 1.0
                || index.number > static_cast<double>(elements.size()))
                throw Failure{"An index lies outside the collection", range};
            const auto spot = elements.begin() + static_cast<std::ptrdiff_t>(index.number - 1.0);
            const auto removed = *spot;
            elements.erase(spot);
            return removed;
        }
        if (selector == "whereToInsert:" && collection.sorted)
        {
            const auto copy = collection;
            std::size_t index = 0;
            while (index < copy.elements.size() && before(copy, copy.elements[index], arguments[0], range))
                ++index;
            return Value::of(static_cast<double>(index + 1));
        }
        if (selector == "addFirst:" && !collection.isSet)
        {
            if (elements.size() >= maximumElements)
                throw Failure{"A collection holds at most 65536 elements", range};
            elements.insert(elements.begin(), arguments[0]);
            return arguments[0];
        }
        // Adding and removing many at once, an
        // element's count, and a removal that answers a block's value when
        // the element is absent.
        if (selector == "addAll:" || selector == "addAllLast:" || selector == "addAllFirst:")
        {
            auto added = elementsOf(arguments[0], range);
            if (selector == "addAllFirst:" && !collection.isSet)
            {
                if (elements.size() + added.size() > maximumElements)
                    throw Failure{"A collection holds at most 65536 elements", range};
                elements.insert(elements.begin(), added.begin(), added.end());
            }
            else
            {
                for (const auto& element : added)
                    add(collection.isSet, elements, element, range);
            }
            return arguments[0];
        }
        if (selector == "removeAll:")
        {
            for (const auto& element : elementsOf(arguments[0], range))
            {
                const auto found = std::ranges::find_if(elements, [&](const Value& held) { return equal(held, element, range); });
                if (found == elements.end())
                    throw Failure{"The collection does not hold an element to remove", range};
                elements.erase(found);
            }
            return arguments[0];
        }
        if (selector == "remove:ifAbsent:")
        {
            const auto found = std::ranges::find_if(elements, [&](const Value& held) { return equal(held, arguments[0], range); });
            if (found == elements.end())
                return callBlock(arguments[1], {}, range);
            const auto removed = *found;
            elements.erase(found);
            return removed;
        }
        // Each element's count as an Association
        // to it, the most counted first.
        if (selector == "sortedCounts" && arguments.empty())
        {
            std::vector<std::pair<Value, std::size_t>> counts;
            std::map<std::string, std::size_t> seen;
            for (const auto& element : elements)
            {
                const auto [found, added] = seen.try_emplace(elementKey(element), counts.size());
                if (added)
                    counts.emplace_back(element, 0);
                ++counts[found->second].second;
            }
            std::ranges::stable_sort(counts, std::greater{}, &std::pair<Value, std::size_t>::second);
            Collection sorted;
            sorted.sorted = true;
            for (const auto& [element, count] : counts)
                sorted.elements.push_back(tagged(associationTag, {Value::of(static_cast<double>(count)), element}));
            return adopt(collections_, collectionsHeld_, std::move(sorted), firstCollection, maximumCollections, "collections",
                range);
        }
        if (selector == "occurrencesOf:")
        {
            return Value::of(static_cast<double>(std::ranges::count_if(elements,
                [&](const Value& held) { return equal(held, arguments[0], range); })));
        }
        if (selector == "removeFirst" || selector == "removeLast")
        {
            empty();
            const auto removed = selector == "removeFirst" ? elements.front() : elements.back();
            if (selector == "removeFirst")
                elements.erase(elements.begin());
            else
                elements.pop_back();
            return removed;
        }
        if (selector == "remove:")
        {
            const auto found = std::ranges::find_if(elements, [&](const Value& element) {
                return equal(element, arguments[0], range);
            });
            if (found == elements.end())
                throw Failure{"The collection does not hold the element to remove", range};
            const auto removed = *found;
            elements.erase(found);
            return removed;
        }
        if (selector == "first" || selector == "last")
        {
            empty();
            return selector == "first" ? elements.front() : elements.back();
        }
        if (selector == "size")
            return Value::of(static_cast<double>(elements.size()));
        if (selector == "isEmpty" || selector == "notEmpty")
            return Value::of(elements.empty() == (selector == "isEmpty"));
        if (selector == "includes:")
        {
            return Value::of(std::ranges::any_of(elements, [&](const Value& element) {
                return equal(element, arguments[0], range);
            }));
        }
        if (selector == "asArray")
            return Value::array(elements);
        if (selector == "do:")
        {
            const auto copy = elements;
            for (const auto& element : copy)
                callBlock(arguments[0], std::array{element}, range);
            return receiver;
        }
        // Conversions, streams and the queries an Array answers act on the
        // elements.
        return sendElements(elements, selector, arguments, range);
    }

    // What any collection answers of its elements: conversions to the other
    // kinds, streams on them, and an Array's
    // messages.
    Value sendElements(const std::vector<Value>& elements, const std::string& selector, std::span<const Value> arguments,
        const SourceRange& range)
    {
        if (selector == "asBag" || selector == "asSet" || selector == "asOrderedCollection"
            || selector == "asSortedCollection" || selector == "asSortedCollection:")
        {
            Collection made;
            made.isSet = selector == "asSet";
            made.bag = selector == "asBag";
            made.sorted = selector.starts_with("asSortedCollection");
            if (selector == "asSortedCollection:")
                made.sortBlock = arguments[0];
            made.elements = elements;
            return makeCollection(std::move(made), range);
        }
        if (selector == "readStream" || selector == "writeStream")
            return makeStream(elements, false, selector == "writeStream", range);
        return sendArray(Value::array(elements), selector, arguments, range);
    }

    [[nodiscard]] bool before(const Collection& collection, const Value& first, const Value& second,
        const SourceRange& range)
    {
        return truth(collection.sortBlock.has_value() ? callBlock(*collection.sortBlock, std::array{first, second}, range)
                                                     : send(first, "<=", std::array{second}, range),
            range);
    }

    // A new collection: a Set's elements made unique, a SortedCollection's
    // put in order.
    Value makeCollection(Collection made, const SourceRange& range)
    {
        if (made.elements.size() > maximumElements)
            throw Failure{"A collection holds at most 65536 elements", range};
        if (made.isSet)
        {
            std::vector<Value> unique;
            for (const auto& element : made.elements)
                add(true, unique, element, range);
            made.elements = std::move(unique);
        }
        if (made.sorted)
        {
            std::vector<Value> ordered;
            for (const auto& element : made.elements)
            {
                const auto place = std::ranges::find_if(ordered, [&](const Value& held) {
                    return !before(made, held, element, range);
                });
                ordered.insert(place, element);
            }
            made.elements = std::move(ordered);
        }
        return adopt(collections_, collectionsHeld_, std::move(made), firstCollection, maximumCollections, "collections",
            range);
    }

    Value makeStream(std::vector<Value> elements, bool text, bool writable, const SourceRange& range)
    {
        if (writable)
            elements.clear();
        ReadStream stream;
        stream.elements = std::move(elements);
        stream.text = text;
        stream.writable = writable;
        return adopt(streams_, streamsHeld_, std::move(stream), firstReadStream, maximumCollections, "streams", range);
    }

    // A Dictionary: values at unique keys, its
    // iteration going over the values.
    Value sendDictionary(const Value& receiver, const std::string& selector, std::span<const Value> arguments,
        const SourceRange& range)
    {
        if (receiver.index == dictionaryClass)
        {
            if (selector != "new" && selector != "new:")
                throw Failure{"Dictionary understands new", range};
            return adopt(dictionaries_, dictionariesHeld_, Dictionary{}, firstDictionary, maximumCollections, "dictionaries",
                range);
        }
        auto& entries = dictionaries_[receiver.index - firstDictionary].entries;
        const auto find = [&](const Value& key) {
            return std::ranges::find_if(entries, [&](const auto& entry) {
                return entry.first.kind == key.kind && equal(entry.first, key, range);
            });
        };
        std::vector<Value> values;
        for (const auto& entry : entries)
            values.push_back(entry.second);
        if (selector == "at:put:")
        {
            if (const auto found = find(arguments[0]); found != entries.end())
                found->second = arguments[1];
            else
            {
                if (entries.size() >= maximumElements)
                    throw Failure{"A Dictionary holds at most 65536 entries", range};
                entries.emplace_back(arguments[0], arguments[1]);
            }
            return arguments[1];
        }
        if (selector == "at:" || selector == "at:ifAbsent:")
        {
            if (const auto found = find(arguments[0]); found != entries.end())
                return found->second;
            if (selector == "at:ifAbsent:")
                return callBlock(arguments[1], {}, range);
            throw Failure{"The Dictionary has no such key", range};
        }
        if (selector == "keyAtValue:" || selector == "keyAtValue:ifAbsent:")
        {
            const auto found = std::ranges::find_if(entries, [&](const auto& entry) {
                return entry.second.kind == arguments[0].kind && equal(entry.second, arguments[0], range);
            });
            if (found != entries.end())
                return found->first;
            if (selector == "keyAtValue:ifAbsent:")
                return callBlock(arguments[1], {}, range);
            throw Failure{"The Dictionary has no such value", range};
        }
        if (selector == "removeKey:" || selector == "removeKey:ifAbsent:")
        {
            const auto found = find(arguments[0]);
            if (found == entries.end())
            {
                if (selector == "removeKey:ifAbsent:")
                    return callBlock(arguments[1], {}, range);
                throw Failure{"The Dictionary has no such key", range};
            }
            const auto value = found->second;
            entries.erase(found);
            return value;
        }
        if (selector == "includesKey:")
            return Value::of(find(arguments[0]) != entries.end());
        if (selector == "keysAndValuesDo:" || selector == "keysDo:")
        {
            const auto copy = entries;
            for (const auto& [key, value] : copy)
            {
                if (selector == "keysDo:")
                    callBlock(arguments[0], std::array{key}, range);
                else
                    callBlock(arguments[0], std::array{key, value}, range);
            }
            return receiver;
        }
        if (selector == "keys")
        {
            Collection keys;
            keys.isSet = true;
            for (const auto& entry : entries)
                keys.elements.push_back(entry.first);
            return makeCollection(std::move(keys), range);
        }
        if (selector == "values")
            return Value::array(std::move(values));
        if (selector == "size")
            return Value::of(static_cast<double>(entries.size()));
        if (selector == "isEmpty" || selector == "notEmpty")
            return Value::of(entries.empty() == (selector == "isEmpty"));
        return sendElements(values, selector, arguments, range);
    }

    void add(bool isSet, std::vector<Value>& elements, const Value& element, const SourceRange& range) const
    {
        if (isSet && std::ranges::any_of(elements, [&](const Value& held) { return equal(held, element, range); }))
            return;
        if (elements.size() >= maximumElements)
            throw Failure{"A collection holds at most 65536 elements", range};
        elements.push_back(element);
    }

    Value sendSelf(const std::string& selector, std::span<const Value> arguments, const SourceRange& range)
    {
        const auto shown = [](const Value& value) {
            if (const auto text = fieldText(value))
                return *text;
            return std::string{value.kind == Value::Kind::Nil ? "nil"
                    : value.kind == Value::Kind::Boolean ? (value.boolean ? "true" : "false")
                                                         : "an object"};
        };
        // Debug: shows the value and answers
        // it; debugWithLabel:value: shows "i = 0" and answers nil.
        if (selector == "debug:" || selector == "debugWithLabel:value:")
        {
            const auto& value = arguments.back();
            const auto line = selector == "debug:" ? shown(value) : shown(arguments.front()) + " = " + shown(value);
            if (!trial_ && debugLines_ != nullptr && debugLines_->size() < maximumDebugLines)
                debugLines_->push_back(line);
            // The script pauses on the value, OK going on and
            // Cancel stopping it.
            if (host_.pausesAtDebug() && !askHost({PromptKind::Confirm, line, {}, {}}, range).accepted)
                throw Failure{"Stopped at " + line, range};
            return selector == "debug:" ? value : Value::nil();
        }
        // Questions for the user, answered by the
        // host, the script stopped while one waits to be asked.
        const auto question = [&](PromptKind kind, std::string initial) -> PromptAnswer {
            if (arguments.front().kind != Value::Kind::String)
                throw Failure{selector + " asks its question in a String", range};
            return askHost({kind, arguments.front().text, std::move(initial), {}}, range);
        };
        if (selector == "confirm:" || selector == "confirmYesNo:")
            return Value::of(question(selector == "confirm:" ? PromptKind::Confirm : PromptKind::ConfirmYesNo, {}).accepted);
        if (selector == "confirm:ifCancel:")
            return question(PromptKind::Confirm, {}).accepted ? Value::of(true)
                                                              : send(arguments[1], "value", std::span<const Value>{}, range);
        if (selector == "getValueFromUser:")
        {
            const auto answer = question(PromptKind::Value, {});
            return answer.accepted ? valueOfTyped(answer.text, range) : Value::nil();
        }
        if (selector == "request:initialAnswer:evaluating:ifCancel:")
        {
            if (arguments[1].kind != Value::Kind::String || arguments[2].kind != Value::Kind::Boolean)
                throw Failure{"request:initialAnswer:evaluating:ifCancel: takes a String, a String and a Boolean", range};
            const auto answer = question(PromptKind::Request, arguments[1].text);
            if (!answer.accepted)
                return send(arguments[3], "value", std::span<const Value>{}, range);
            if (!arguments[2].boolean)
                return Value::string(answer.text);
            return Value::array({Value::string(answer.text), valueOfTyped(answer.text, range)});
        }
        // Self addSound: son atTime: 0 s plays son
        // as son start: 0 s does.
        if (selector == "addSound:atTime:")
        {
            const std::array timed{arguments[1]};
            return send(arguments[0], "start:", timed, range);
        }
        throw Failure{"A script does not understand " + selector, range};
    }

    // The host's answer to a question; the script stops while it has still
    // to ask it.
    PromptAnswer askHost(Prompt prompt, const SourceRange& range)
    {
        if (trial_)
            throw TrialStop{true};
        auto answer = host_.ask(prompt);
        if (!answer.has_value())
        {
            if (pendingPrompt_ != nullptr)
                *pendingPrompt_ = prompt;
            throw Failure{"The script waits for an answer to " + prompt.question, range};
        }
        return std::move(*answer);
    }

    // A file from the user, by the host's file
    // dialog showing the files of the types given; the Filename chosen, or
    // the cancel block's value.
    Value sendFilenameClass(const std::string& selector, std::span<const Value> arguments, const SourceRange& range)
    {
        const auto open = selector == "getFileWithPrompt:fileTypes:ifCancel:";
        if (!open && selector != "putFileWithPrompt:initially:fileTypes:ifCancel:")
            throw Failure{"Filename understands getFileWithPrompt:fileTypes:ifCancel: and "
                          "putFileWithPrompt:initially:fileTypes:ifCancel:",
                range};
        if (arguments[0].kind != Value::Kind::String)
            throw Failure{selector + " asks its question in a String", range};
        const auto& types = arguments[open ? 1 : 2];
        std::vector<std::string> extensions;
        for (const auto& type : types.kind == Value::Kind::Array ? *types.elements : std::vector<Value>{types})
        {
            for (const auto& extension : extensionsOfType(type, range))
                if (std::ranges::find(extensions, extension) == extensions.end())
                    extensions.push_back(extension);
        }
        const auto answer = askHost({open ? PromptKind::OpenFile : PromptKind::SaveFile, arguments[0].text,
                                        open ? std::string{} : pathOf(arguments[1], selector, range), std::move(extensions)},
            range);
        if (!answer.accepted)
            return send(arguments[open ? 2 : 3], "value", std::span<const Value>{}, range);
        return newFilename(answer.text, range);
    }

    // What a typed answer evaluates to, as Evaluate
    // answers it; a block or an object of its own becomes its printString.
    Value valueOfTyped(const std::string& text, const SourceRange& range)
    {
        const auto parsed = talas::project::smalltalk::parse(text);
        if (!parsed.ok())
            throw Failure{"The answer typed is no value: " + parsed.diagnostic->message, range};
        Interpreter typed(*parsed.program, host_);
        Value value;
        try
        {
            value = typed.runBody(0, nullptr);
        }
        catch (const Unwind& unwind)
        {
            value = unwind.value;
        }
        catch (const Failure& failure)
        {
            throw Failure{"The answer typed failed: " + failure.message, range};
        }
        return value.kind == Value::Kind::Object || value.kind == Value::Kind::Block ? Value::string(typed.printString(value))
                                                                                     : value;
    }

public:
    std::vector<std::string>* debugLines_ = nullptr;
    std::vector<XYGraph>* graphs_ = nullptr;
    std::optional<Prompt>* pendingPrompt_ = nullptr;

private:
    static constexpr std::size_t maximumDebugLines = 1024;
    static constexpr std::size_t scriptSelf = std::numeric_limits<std::size_t>::max() - 20;

    // Objects of the interpreter's own, beyond any host's.
    static constexpr std::size_t arrayClass = std::numeric_limits<std::size_t>::max() - 17;
    static constexpr std::size_t maximumElements = 65536;
    static constexpr std::size_t randomClass = std::numeric_limits<std::size_t>::max() - 16;
    static constexpr std::size_t firstRandom = std::size_t{1} << 40;
    static constexpr std::size_t maximumRandoms = 1024;
    static constexpr std::size_t oneOverFClass = std::numeric_limits<std::size_t>::max() - 18;
    static constexpr std::size_t firstOneOverF = std::size_t{1} << 41;

    static constexpr std::size_t readStreamClass = std::numeric_limits<std::size_t>::max() - 21;
    static constexpr std::size_t firstReadStream = std::size_t{1} << 43;
    static constexpr std::size_t orderedCollectionClass = std::numeric_limits<std::size_t>::max() - 22;
    static constexpr std::size_t setClass = std::numeric_limits<std::size_t>::max() - 23;
    static constexpr std::size_t firstCollection = std::size_t{1} << 44;
    static constexpr std::size_t maximumCollections = 1024;

    static constexpr std::size_t writeStreamClass = std::numeric_limits<std::size_t>::max() - 24;
    static constexpr std::size_t readWriteStreamClass = std::numeric_limits<std::size_t>::max() - 25;
    static constexpr std::size_t bagClass = std::numeric_limits<std::size_t>::max() - 26;
    static constexpr std::size_t sortedCollectionClass = std::numeric_limits<std::size_t>::max() - 27;
    static constexpr std::size_t dictionaryClass = std::numeric_limits<std::size_t>::max() - 28;
    static constexpr std::size_t stringClass = std::numeric_limits<std::size_t>::max() - 29;
    static constexpr std::size_t floatClass = std::numeric_limits<std::size_t>::max() - 30;
    static constexpr std::size_t dateClass = std::numeric_limits<std::size_t>::max() - 31;
    static constexpr std::size_t timeClass = std::numeric_limits<std::size_t>::max() - 32;
    static constexpr std::size_t pointTag = std::numeric_limits<std::size_t>::max() - 34;
    static constexpr std::size_t complexTag = std::numeric_limits<std::size_t>::max() - 35;
    static constexpr std::size_t dateTag = std::numeric_limits<std::size_t>::max() - 36;
    static constexpr std::size_t timeTag = std::numeric_limits<std::size_t>::max() - 37;
    static constexpr std::size_t filenameTag = std::numeric_limits<std::size_t>::max() - 38;
    static constexpr std::size_t associationTag = std::numeric_limits<std::size_t>::max() - 39;
    static constexpr std::size_t bindingListClass = std::numeric_limits<std::size_t>::max() - 40;
    static constexpr std::size_t pointClass = std::numeric_limits<std::size_t>::max() - 41;
    static constexpr std::size_t rectangleTag = std::numeric_limits<std::size_t>::max() - 42;
    static constexpr std::size_t rectangleClass = std::numeric_limits<std::size_t>::max() - 43;
    static constexpr std::size_t hostDriverInterfaceClass = std::numeric_limits<std::size_t>::max() - 44;
    static constexpr std::size_t fileTypeFilterTag = std::numeric_limits<std::size_t>::max() - 45;
    static constexpr std::size_t classTag = std::numeric_limits<std::size_t>::max() - 46;
    static constexpr std::size_t preferencesClass = std::numeric_limits<std::size_t>::max() - 47;
    static constexpr std::size_t xyGraphViewClass = std::numeric_limits<std::size_t>::max() - 48;
    static constexpr std::size_t filenameClass = std::numeric_limits<std::size_t>::max() - 49;
    static constexpr std::size_t imageTag = std::numeric_limits<std::size_t>::max() - 50;
    static constexpr std::size_t maximumGraphs = 64;
    static constexpr std::size_t maximumGraphPoints = 4096;
    static constexpr std::size_t samplesFileClass = std::numeric_limits<std::size_t>::max() - 33;
    static constexpr std::size_t firstSamplesFile = std::size_t{1} << 50;
    static constexpr std::size_t maximumSamplesFiles = 1024;
    static constexpr std::size_t firstDictionary = std::size_t{1} << 45;



    static constexpr std::size_t markovChainClass = std::numeric_limits<std::size_t>::max() - 19;
    static constexpr std::size_t firstMarkovChain = std::size_t{1} << 42;
    static constexpr std::size_t maximumChainLength = 64;
    static constexpr std::size_t maximumMarkovTraining = 262144;

    struct MarkovChain
    {
        // The elements a run holds: chainLength - 1.
        std::size_t order = 0;
        // The training sequence's distinct elements, and the sequence as
        // their indices.
        std::vector<Value> elements;
        std::vector<std::uint32_t> training;
        // The elements following each run, in the order the training
        // sequence first has them, with their cumulative counts.
        std::map<std::vector<std::uint32_t>, std::vector<std::pair<std::uint32_t, std::size_t>>> following;
    };

    struct OneOverF
    {
        std::uint32_t state = 1u;
        std::uint64_t count = 0;
        std::vector<double> rows;
    };

    const Program* program_;
    Host& host_;
    // Whether integers and ratios are exact, as since format 121.
    bool exactNumbers_ = true;
    // Whether a long collection prints abbreviated, as all but printAllOn:
    // print it.
    bool abbreviates_ = true;
    std::vector<Closure> closures_;
    std::vector<std::size_t> freeClosures_;
    std::size_t sweepClosuresAt_ = 1024;
    std::vector<Value> cascadeReceivers_;
    std::vector<std::uint32_t> randoms_;
    std::vector<SampleFileInfo> samplesFiles_;
    std::vector<OneOverF> oneOverFs_;
    std::vector<MarkovChain> markovChains_;
    std::vector<ReadStream> streams_;
    std::vector<Collection> collections_;
    std::vector<Dictionary> dictionaries_;
    // Each object's token, held by the Values that refer to it.
    std::vector<std::weak_ptr<const std::vector<Value>>> randomsHeld_;
    std::vector<std::weak_ptr<const std::vector<Value>>> oneOverFsHeld_;
    std::vector<std::weak_ptr<const std::vector<Value>>> samplesFilesHeld_;
    std::vector<std::weak_ptr<const std::vector<Value>>> streamsHeld_;
    std::vector<std::weak_ptr<const std::vector<Value>>> collectionsHeld_;
    std::vector<std::weak_ptr<const std::vector<Value>>> dictionariesHeld_;
    std::size_t markovTrainingElements_ = 0;
    std::size_t unlikeKeys_ = 0;
    std::size_t steps_ = 0;
    std::size_t depth_ = 0;
    // recognizes: trying a message.
    bool trial_ = false;
};
}

RunResult run(const Program& program, Host& host)
{
    RunResult result;
    try
    {
        Interpreter interpreter(program, host);
        interpreter.debugLines_ = &result.debugLines;
        interpreter.graphs_ = &result.graphs;
        // The host may send back to the interpreter while it runs.
        struct Attached
        {
            Host& host;
            Messenger* previous;
            ~Attached() { host.messenger_ = previous; }
        } attached{host, host.messenger_};
        host.messenger_ = &interpreter;
        struct Saved
        {
            Interpreter& interpreter;
            ~Saved() { interpreter.saveFiles(); }
        } saved{interpreter};
        result.value = interpreter.runBody(0, nullptr);
    }
    catch (const Unwind& unwind)
    {
        result.value = unwind.value;
    }
    catch (const Failure& failure)
    {
        result.diagnostic = Diagnostic{failure.message, failure.range};
    }
    catch (const std::bad_alloc&)
    {
        result.diagnostic = Diagnostic{"The script ran out of memory", {}};
    }
    return result;
}

Evaluation evaluate(std::string_view source, Host& host)
{
    Evaluation evaluation;
    const auto parsed = parse(source);
    if (!parsed.ok())
    {
        evaluation.diagnostic = parsed.diagnostic;
        return evaluation;
    }
    try
    {
        Interpreter interpreter(*parsed.program, host);
        interpreter.debugLines_ = &evaluation.debugLines;
        interpreter.graphs_ = &evaluation.graphs;
        interpreter.pendingPrompt_ = &evaluation.pendingPrompt;
        struct Attached
        {
            Host& host;
            Messenger* previous;
            ~Attached() { host.messenger_ = previous; }
        } attached{host, host.messenger_};
        host.messenger_ = &interpreter;
        struct Saved
        {
            Interpreter& interpreter;
            ~Saved() { interpreter.saveFiles(); }
        } saved{interpreter};
        Value value;
        try
        {
            value = interpreter.runBody(0, nullptr);
        }
        catch (const Unwind& unwind)
        {
            value = unwind.value;
        }
        evaluation.printed = interpreter.printString(value);
    }
    catch (const Failure& failure)
    {
        evaluation.diagnostic = Diagnostic{failure.message, failure.range};
    }
    catch (const std::bad_alloc&)
    {
        evaluation.diagnostic = Diagnostic{"The script ran out of memory", {}};
    }
    return evaluation;
}

EvaluatedText evaluateSelection(
    std::string_view text, std::size_t selectionStart, std::size_t selectionEnd, Host& host)
{
    EvaluatedText result;
    result.text = std::string{text};
    selectionEnd = std::min(selectionEnd, text.size());
    selectionStart = std::min(selectionStart, selectionEnd);
    if (selectionStart == selectionEnd)
    {
        const auto before = text.substr(0, selectionStart).find_last_of("\r\n");
        const auto after = text.find_first_of("\r\n", selectionEnd);
        selectionStart = before == std::string_view::npos ? 0 : before + 1;
        selectionEnd = after == std::string_view::npos ? text.size() : after;
    }
    const auto evaluation = evaluate(text.substr(selectionStart, selectionEnd - selectionStart), host);
    result.debugLines = evaluation.debugLines;
    result.graphs = evaluation.graphs;
    result.pendingPrompt = evaluation.pendingPrompt;
    if (!evaluation.ok())
    {
        result.diagnostic = evaluation.diagnostic;
        if (result.diagnostic.has_value())
            result.diagnostic->range.offset += selectionStart;
        return result;
    }
    const auto inserted = " " + evaluation.printed;
    result.text.insert(selectionEnd, inserted);
    result.resultStart = selectionEnd + 1;
    result.resultEnd = selectionEnd + inserted.size();
    return result;
}

struct Session::State
{
    explicit State(Host& host)
        : host(host)
    {
    }

    Host& host;
    // Every program run, once each, since its blocks may run later.
    std::set<std::shared_ptr<const Program>> programs;
    std::unique_ptr<Interpreter> interpreter;
};

Session::Session(Host& host)
    : state_(std::make_unique<State>(host))
{
}

Session::~Session() = default;

RunResult Session::run(std::shared_ptr<const Program> program)
{
    RunResult result;
    if (program == nullptr)
    {
        result.diagnostic = Diagnostic{"No program to run", {}};
        return result;
    }
    try
    {
        state_->programs.insert(program);
        if (state_->interpreter == nullptr)
            state_->interpreter = std::make_unique<Interpreter>(*program, state_->host);
        auto& interpreter = *state_->interpreter;
        interpreter.begin(*program);
        interpreter.debugLines_ = &result.debugLines;
        interpreter.graphs_ = &result.graphs;
        struct Attached
        {
            Host& host;
            Messenger* previous;
            ~Attached() { host.messenger_ = previous; }
        } attached{state_->host, state_->host.messenger_};
        state_->host.messenger_ = &interpreter;
        result.value = interpreter.runBody(0, nullptr);
        interpreter.debugLines_ = nullptr;
        interpreter.graphs_ = nullptr;
    }
    catch (const Unwind& unwind)
    {
        result.value = unwind.value;
    }
    catch (const Failure& failure)
    {
        result.diagnostic = Diagnostic{failure.message, failure.range};
    }
    catch (const std::bad_alloc&)
    {
        result.diagnostic = Diagnostic{"The script ran out of memory", {}};
    }
    if (state_->interpreter != nullptr)
    {
        state_->interpreter->debugLines_ = nullptr;
        state_->interpreter->saveFiles();
    }
    return result;
}

namespace
{
// The directories a path lies in, from the top down: a/b/c gives a and a/b.
std::vector<std::string> parentsOf(std::string_view path)
{
    std::vector<std::string> parents;
    for (auto slash = path.find('/'); slash != std::string_view::npos; slash = path.find('/', slash + 1))
    {
        if (slash > 0)
            parents.emplace_back(path.substr(0, slash));
    }
    return parents;
}
}

std::optional<std::string> MemoryFileSystem::read(std::string_view path) const
{
    const auto found = files_.find(path);
    if (found == files_.end())
        return std::nullopt;
    return found->second;
}

void MemoryFileSystem::write(std::string_view path, std::string contents)
{
    for (auto& parent : parentsOf(path))
        directories_.insert(std::move(parent));
    files_.insert_or_assign(std::string{path}, std::move(contents));
    const auto now = clock_ ? clock_() : std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    dates_.insert_or_assign(std::string{path}, FileDates{now, now, now});
}

std::optional<FileDates> MemoryFileSystem::dates(std::string_view path) const
{
    const auto found = dates_.find(path);
    if (found == dates_.end() || !files_.contains(path))
        return std::nullopt;
    return found->second;
}

std::uint32_t Host::freshSeed()
{
    static std::random_device entropy;
    return entropy();
}

void EvaluationHost::answerFrom(std::vector<PromptAnswer> answers, std::vector<std::uint32_t> seeds)
{
    answering_ = true;
    answers_ = std::move(answers);
    asked_ = 0;
    seeds_ = std::move(seeds);
    seeded_ = 0;
}

std::optional<PromptAnswer> EvaluationHost::ask(const Prompt& prompt)
{
    if (!answering_)
        return Host::ask(prompt);
    if (asked_ < answers_.size())
        return answers_[asked_++];
    return std::nullopt;
}

std::uint32_t EvaluationHost::freshSeed()
{
    if (!answering_)
        return Host::freshSeed();
    if (seeded_ == seeds_.size())
        seeds_.push_back(Host::freshSeed());
    return seeds_[seeded_++];
}

std::string withRandomNewRepeatable(std::string_view text)
{
    constexpr std::string_view unseeded = "Random new";
    const auto isName = [](char character) {
        return std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_' || character == ':';
    };
    std::string result;
    std::size_t copied = 0;
    for (auto found = text.find(unseeded); found != std::string_view::npos; found = text.find(unseeded, found + 1))
    {
        const auto end = found + unseeded.size();
        if ((found > 0 && isName(text[found - 1])) || (end < text.size() && isName(text[end])))
            continue;
        result.append(text.substr(copied, found - copied));
        result += "(Random newForTalasWithSeed: 0)";
        copied = end;
    }
    result.append(text.substr(copied));
    return result;
}

std::string* MemoryFileSystem::find(std::string_view path)
{
    const auto found = files_.find(path);
    return found == files_.end() ? nullptr : &found->second;
}

bool MemoryFileSystem::remove(std::string_view path)
{
    if (const auto found = files_.find(path); found != files_.end())
    {
        files_.erase(found);
        return true;
    }
    const auto found = directories_.find(path);
    if (found == directories_.end() || !list(path).empty())
        return false;
    directories_.erase(found);
    return true;
}

bool MemoryFileSystem::isDirectory(std::string_view path) const
{
    return path.empty() || path == "/" || directories_.contains(path);
}

void MemoryFileSystem::makeDirectory(std::string_view path)
{
    for (auto& parent : parentsOf(path))
        directories_.insert(std::move(parent));
    if (!path.empty())
        directories_.insert(std::string{path});
}

std::vector<std::string> MemoryFileSystem::list(std::string_view directory) const
{
    const auto prefix = directory.empty() ? std::string{} : directory.ends_with('/') ? std::string{directory}
                                                                                     : std::string{directory} + "/";
    std::set<std::string> names;
    const auto take = [&](const std::string& path) {
        if (path.starts_with(prefix) && path.size() > prefix.size()
            && path.find('/', prefix.size()) == std::string::npos)
            names.insert(path.substr(prefix.size()));
    };
    for (const auto& [path, contents] : files_)
        take(path);
    for (const auto& path : directories_)
        take(path);
    return {names.begin(), names.end()};
}

std::optional<Value> declaredVariable(
    const ProjectVariableLibrary& library, std::string_view name, std::string_view definitionId)
{
    if (name.starts_with('?'))
        name.remove_prefix(1);
    const ProjectVariableDeclaration* found = nullptr;
    for (const auto& declaration : library.variables)
    {
        if (declaration.variableId != name)
            continue;
        // The declaration of the Sound the script is in, else the
        // project's, else any.
        const auto owned = !definitionId.empty() && declaration.owner.kind == ProjectVariableOwnerKind::SoundDefinition
            && declaration.owner.definitionId == definitionId;
        if (found == nullptr || owned
            || (declaration.owner.kind == ProjectVariableOwnerKind::Project
                && found->owner.kind != ProjectVariableOwnerKind::Project))
        {
            found = &declaration;
        }
        if (owned)
            break;
    }
    if (found == nullptr || !std::isfinite(found->initialValue))
        return std::nullopt;
    auto value = Value::expression("@" + std::string{name});
    value.known = true;
    value.number = found->initialValue;
    switch (found->domain)
    {
    case talas::expr::ValueDomain::Hertz:
        value.unit = Unit::Hertz;
        break;
    case talas::expr::ValueDomain::Seconds:
        value.unit = Unit::Seconds;
        break;
    case talas::expr::ValueDomain::NoteNumber:
        value.unit = Unit::NoteNumber;
        break;
    case talas::expr::ValueDomain::Samples:
        value.unit = Unit::Samples;
        break;
    case talas::expr::ValueDomain::Scalar:
        break;
    }
    return value;
}

Value numberOf(const Value& value)
{
    if (value.kind == Value::Kind::Expression && value.known)
        return Value::of(value.number, value.unit);
    return value;
}

std::optional<std::string> fieldText(const Value& value)
{
    switch (value.kind)
    {
    case Value::Kind::Number:
    {
        auto text = numberText(value.number);
        if (const auto unit = unitText(value.unit); !unit.empty())
        {
            text += ' ';
            text += unit;
        }
        return text;
    }
    case Value::Kind::Expression:
    case Value::Kind::String:
    case Value::Kind::Symbol:
        return value.text;
    case Value::Kind::Array:
    {
        // A literal Array of numbers, as a field's #(1 1 1 4).
        if (value.elements == nullptr)
            return std::nullopt;
        std::string text = "#(";
        for (const auto& element : *value.elements)
        {
            if (element.kind != Value::Kind::Number || element.unit != talas::expr::Unit::None)
                return std::nullopt;
            if (text.size() > 2)
                text += ' ';
            text += numberText(element.number);
        }
        return text + ")";
    }
    default:
        return std::nullopt;
    }
}

namespace
{
// A program in a field knows no names and no objects of its own.
class FieldHost final : public Host
{
public:
    FieldHost(bool exactNumbers, talas::expr::RoundingRule rounding)
        : exactNumbers_(exactNumbers)
        , rounding_(rounding)
    {
    }

    [[nodiscard]] bool exactNumbers() override { return exactNumbers_; }
    [[nodiscard]] talas::expr::RoundingRule rounding() override { return rounding_; }

    [[nodiscard]] std::optional<Value> variable(std::string_view) override
    {
        return std::nullopt;
    }

    [[nodiscard]] std::optional<Value> send(
        const Value&, std::string_view selector, std::span<const Value>, std::string& error) override
    {
        error = "A field's program does not understand " + std::string{selector};
        return std::nullopt;
    }

private:
    bool exactNumbers_ = true;
    talas::expr::RoundingRule rounding_ = talas::expr::RoundingRule::Smalltalk;
};
}

std::optional<std::string> fieldProgramText(std::string_view text, bool exactNumbers, talas::expr::RoundingRule rounding)
{
    // Val's own programs, of temporaries assigned once and
    // EventVariables, stay Val's, when what they give is Val; a
    // block called with its arguments, as a faderMaker, is
    // Smalltalk's to run.
    const auto val = talas::expr::fieldProgramExpression(text);
    if ((val.expression.has_value()
            && talas::expr::parse(*val.expression, talas::expr::ExpressionLanguageVersion::ValV1).ok())
        || (!val.expression.has_value() && val.error.empty()))
        return std::nullopt;
    const auto parsed = parse(text);
    if (!parsed.ok())
        return std::nullopt;
    FieldHost host{exactNumbers, rounding};
    const auto ran = run(*parsed.program, host);
    if (!ran.ok()
        || (ran.value.kind != Value::Kind::Expression && ran.value.kind != Value::Kind::Number)
        || (ran.value.kind == Value::Kind::Expression && ran.value.known))
    {
        return std::nullopt;
    }
    return fieldText(ran.value);
}

std::optional<std::string> listFieldNumbers(std::string_view text, bool exactNumbers, talas::expr::RoundingRule rounding)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0)
        text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0)
        text.remove_suffix(1);
    if (text.size() < 2 || text.front() != '{' || text.back() != '}')
        return std::nullopt;
    // The braces hold the whole field, not the first of its elements.
    std::size_t depth = 0;
    for (std::size_t index = 0; index + 1 < text.size(); ++index)
    {
        if (text[index] == '\'')
        {
            const auto close = text.find('\'', index + 1);
            if (close == std::string_view::npos)
                return std::nullopt;
            index = close;
            continue;
        }
        depth += text[index] == '{' ? 1 : 0;
        if (text[index] == '}' && --depth == 0)
            return std::nullopt;
    }
    const auto parsed = parse(text.substr(1, text.size() - 2));
    if (!parsed.ok())
        return std::nullopt;
    FieldHost host{exactNumbers, rounding};
    const auto ran = run(*parsed.program, host);
    if (!ran.ok() || ran.value.kind != Value::Kind::Array || ran.value.elements == nullptr
        || ran.value.elements->empty())
    {
        return std::nullopt;
    }
    std::string numbers;
    for (const auto& element : *ran.value.elements)
    {
        const auto written = element.kind == Value::Kind::Number ? fieldText(element) : std::nullopt;
        if (!written.has_value())
            return std::nullopt;
        if (!numbers.empty())
            numbers += ' ';
        numbers += *written;
    }
    return numbers;
}
}
