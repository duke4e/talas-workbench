#include "talas/expr/Expression.h"

#include "talas/core/FromChars.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <charconv>
#include <functional>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <utility>

namespace talas::expr
{
static_assert(
    ExpressionLimits::maximumInstructions
        <= std::numeric_limits<std::uint16_t>::max());
static_assert(
    ExpressionLimits::maximumEventReferences
        <= std::numeric_limits<std::uint16_t>::max());
static_assert(
    ExpressionLimits::maximumVariableReferences
        <= std::numeric_limits<std::uint16_t>::max());
static_assert(
    ExpressionLimits::maximumIdentifiers
        <= std::numeric_limits<std::uint16_t>::max());
static_assert(
    ExpressionLimits::maximumListElements
        <= std::numeric_limits<std::uint8_t>::max());
static_assert(
    ExpressionLimits::maximumNodes
        <= std::numeric_limits<std::uint16_t>::max());
static_assert(
    ExpressionLimits::maximumDefinitions
        <= std::numeric_limits<std::uint16_t>::max());
static_assert(
    ExpressionLimits::maximumDefinitionLocals
        <= std::numeric_limits<std::uint16_t>::max());
static_assert(
    ExpressionLimits::maximumStateSlots
        <= std::numeric_limits<std::uint16_t>::max());

// The mark between # and ( of the Array a braced collect: program writes
// out, each element of which runs on every tick; nothing else writes it.
constexpr char eagerArrayMark = '\x01';

namespace detail
{
enum class NodeKind
{
    Literal,
    EventReference,
    VariableReference,
    IdentifierReference,
    FormalReference,
    Unary,
    UnitApplication,
    Binary,
    Comparison,
    Call,
    ListIndex,
    ArrayMap,
    SeriesIndex,
    UserCall
};

enum class ComparisonOp
{
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    Equal,
    NotEqual
};

enum class FunctionOp
{
    Abs,
    Sqrt,
    Log,
    Exp,
    Clamp,
    Map,
    If,
    Previous,
    Accumulate,
    SampleHold,
    Ramp,
    // Val messages.
    Log10,
    Log2,
    Exp2,
    Squared,
    Reciprocal,
    Sign,
    Truncate,
    Round,
    ClipTo01,
    WrapTo02,
    Cos,
    Sin,
    NormCos,
    NormSin,
    LogicValue,
    RemoveUnits,
    Min,
    Max,
    RoundTo,
    ArrayIndex,
    // Smalltalk's messages to numbers.
    Floor,
    Ceiling,
    ArcTan,
    ArcSin,
    ArcCos,
    Tan,
    Sinc,
    TruncateTo,
    CeilingTo,
    MaxAbs,
    MinAbs,
    LogBase,
    FloorLog,
    Between,
    // Val functions of time and triggers.
    TimeFunction,
    Metronome,
    RandomInterval,
    NextRandom,
    Smooth,
    // Val trigger messages, seeded and chaotic random numbers and
    // trackers.
    CountTriggers,
    TriggerEvery,
    Toggle,
    GateWhen,
    TriggerDuration,
    ChangedReset,
    SustainBy,
    NextRandomSeeded,
    Chaotic,
    TrackHold,
    Initially,
    // A value that reads what it gave last time (a SoundToGlobalController
    // reading its own Event Value).
    Recurrence,
    // An EventVariable read and assigned, an
    // expression its temporary shares, expressions in sequence, one updated
    // for its state only, a variable read every millisecond, and nil.
    EventVariableLoad,
    EventVariableStore,
    Shared,
    Sequence,
    UpdateInternalState,
    EveryMillisecond,
    Nothing,
    // A bee following the queen.
    SwarmFollow,
    // Format 156: rounded as the table rounding has it, 0.5 added and
    // truncated toward negative infinity.
    RoundHalfUp,
    // Format 156: inSOSPitch reading a number without units as a fraction
    // of 22050 hz, and hertz or a pitch on the pitch scale.
    LinearUnitlessSOSPitch,
    // The MIDI timing clock's trigger, once for each
    // message a renderer counts in midiTimingClockCountEventName; its
    // argument is !TimingClock's stand-in, given until the first (number
    // 1), or 0 for `MIDITimingClock (number 0).
    MidiTimingClock
};

struct ExpressionNode
{
    NodeKind kind = NodeKind::Literal;
    SourceRange range;
    double number = 0.0;
    Unit unit = Unit::None;
    std::string name;
    UnaryOp unaryOp = UnaryOp::Negate;
    BinaryOp binaryOp = BinaryOp::Add;
    ComparisonOp comparisonOp = ComparisonOp::Equal;
    FunctionOp functionOp = FunctionOp::Abs;
    NodeId left = 0;
    NodeId right = 0;
    std::array<NodeId, 5> arguments{};
    std::uint8_t argumentCount = 0;
    std::uint16_t listElementOffset = 0;
    std::uint8_t listElementCount = 0;
    std::uint16_t referenceIndex = 0;

    bool operator==(const ExpressionNode&) const = default;
};

enum class InstructionOp
{
    PushLiteral,
    ResolveEvent,
    ResolveVariable,
    ResolveIdentifier,
    Negate,
    ApplyUnit,
    Add,
    Subtract,
    Multiply,
    Divide,
    Remainder,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    Equal,
    NotEqual,
    Abs,
    Sqrt,
    Log,
    Exp,
    Clamp,
    Map,
    ArrayMap,
    SelectList,
    ValidateSequenceIndex,
    ValidateSequenceCount,
    CheckSequenceBounds,
    EvaluateSeries,
    StoreLocal,
    LoadLocal,
    // Pops an index and pushes that local of the target locals from
    // referenceIndex.
    PickLocal,
    ValidateDomain,
    JumpIfFalse,
    Jump,
    Previous,
    Accumulate,
    SampleHold,
    Ramp,
    Power,
    IntegerDivide,
    Modulo,
    Log10,
    Log2,
    Exp2,
    Squared,
    Reciprocal,
    Sign,
    Truncate,
    Round,
    ClipTo01,
    WrapTo02,
    Cos,
    Sin,
    NormCos,
    NormSin,
    LogicValue,
    RemoveUnits,
    Min,
    Max,
    RoundTo,
    ArrayIndex,
    Floor,
    Ceiling,
    ArcTan,
    ArcSin,
    ArcCos,
    Tan,
    Sinc,
    TruncateTo,
    CeilingTo,
    MaxAbs,
    MinAbs,
    LogBase,
    FloorLog,
    Between,
    TimeFunction,
    Metronome,
    RandomInterval,
    NextRandom,
    Smooth,
    CountTriggers,
    TriggerEvery,
    Toggle,
    GateWhen,
    TriggerDuration,
    ChangedReset,
    SustainBy,
    NextRandomSeeded,
    Chaotic,
    TrackHold,
    Initially,
    RecurrenceLoad,
    RecurrenceStore,
    EventVariableLoad,
    EventVariableStore,
    Sequence,
    GateBegin,
    GateEnd,
    SwarmFollow,
    RoundHalfUp,
    LinearUnitlessSOSPitch,
    MidiTimingClock
};

struct Instruction
{
    InstructionOp op = InstructionOp::PushLiteral;
    SourceRange range;
    double number = 0.0;
    Unit unit = Unit::None;
    std::uint16_t referenceIndex = 0;
    std::uint16_t target = 0;
    ValueDomain domain = ValueDomain::Scalar;
    std::optional<std::size_t> definitionIndex;

    bool operator==(const Instruction&) const = default;
};

struct ListDispatch
{
    std::array<std::uint16_t,
               ExpressionLimits::maximumListElements> targets{};
    std::uint8_t elementCount = 0;

    bool operator==(const ListDispatch&) const = default;
};

struct ExpressionStorage
{
    ExpressionLanguageVersion languageVersion =
        ExpressionLanguageVersion::LegacyV1;
    NodeId root = 0;
    std::vector<ExpressionNode> nodes;
    std::vector<NodeId> listElements;
    std::vector<std::string> eventReferences;
    std::vector<std::string> variableReferences;
    std::vector<VariableReferenceMetadata> variableReferenceMetadata;
    std::vector<std::string> identifiers;
    std::vector<Instruction> instructions;
    std::vector<ListDispatch> listDispatches;
    std::size_t maximumStackDepth = 0;
    std::size_t localSlotCount = 0;
    std::size_t stateSlotCount = 0;
    bool usesTime = false;
    bool startsFromItsStartingValues = false;
    // A Val source of a project before format 119, whose time
    // functions run on through branches not taken.
    bool timeRunsOnInSkippedBranches = false;
    std::optional<std::size_t> voiceNumberInstruction;
};
}

namespace
{
constexpr std::string_view voiceNumberIdentifier = "?VoiceNumber";
// The comment withTimeRunningOnInSkippedBranches starts a source with.
constexpr std::string_view timeRunsOnMarker = "\"time runs on in branches not taken\" ";
// The comment withWholeSoundClocks starts a source with.
constexpr std::string_view wholeSoundClocksMarker = "\"RealTime and Time are the whole Sound's clocks\" ";

enum class TokenKind
{
    End,
    Number,
    Identifier,
    Event,
    Variable,
    Plus,
    Minus,
    Star,
    Slash,
    Remainder,
    Ramp,
    LeftParen,
    RightParen,
    LeftBracket,
    RightBracket,
    Comma,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    EqualEqual,
    NotEqual,
    InvalidEvent,
    // A backquote without an Event Source's name, or a MIDI controller past
    // 127.
    InvalidEventSource,
    InvalidVariable,
    InvalidVoiceNumber,
    InvalidCharacter,
    // Val.
    Keyword,
    Power,
    IntegerDivide,
    Modulo,
    ArrayStart,
    LeftBrace,
    RightBrace,
    At,
    // An EventVariable's assignments, keeping the value or not.
    AssignKeep,
    AssignPop,
    // The Event Source `MIDITimingClock, which
    // reads no Event Value.
    MidiTimingClockSource
};

enum class ReferenceKind
{
    Event,
    Variable,
    Identifier
};

struct Token
{
    TokenKind kind = TokenKind::End;
    std::string_view text;
    SourceRange range;
};

[[nodiscard]] bool isAsciiWhitespace(char value) noexcept
{
    return value == ' ' || value == '\t' || value == '\n'
        || value == '\r';
}

[[nodiscard]] bool isAsciiDigit(char value) noexcept
{
    return value >= '0' && value <= '9';
}

[[nodiscard]] bool isIdentifierStart(char value) noexcept
{
    return (value >= 'a' && value <= 'z')
        || (value >= 'A' && value <= 'Z') || value == '_';
}

[[nodiscard]] bool isIdentifierContinue(char value) noexcept
{
    return isIdentifierStart(value) || isAsciiDigit(value);
}

[[nodiscard]] SourceRange rangeFrom(
    std::size_t start,
    std::size_t end) noexcept
{
    return {start, end >= start ? end - start : 0};
}

[[nodiscard]] SourceRange joinRanges(
    SourceRange first,
    SourceRange second) noexcept
{
    const auto start = std::min(first.offset, second.offset);
    const auto end = std::max(
        first.offset + first.length,
        second.offset + second.length);
    return rangeFrom(start, end);
}

// The 128 built-in MIDI controller Event Values, cc00 to cc127, kept for
// the tokens of Event Sources that name them.
struct ControllerEventNames
{
    std::array<std::array<char, 5>, 128> text{};
    std::array<std::uint8_t, 128> size{};
};

[[nodiscard]] constexpr ControllerEventNames makeControllerEventNames() noexcept
{
    ControllerEventNames names;
    for (unsigned number = 0; number < 128; ++number)
    {
        auto& text = names.text[number];
        text[0] = 'c';
        text[1] = 'c';
        std::uint8_t size = 2;
        if (number >= 100)
            text[size++] = static_cast<char>('0' + number / 100);
        text[size++] = static_cast<char>('0' + number / 10 % 10);
        text[size++] = static_cast<char>('0' + number % 10);
        names.size[number] = size;
    }
    return names;
}

inline constexpr ControllerEventNames controllerEventNames = makeControllerEventNames();

// The Event Value an Event Source's token reads: a view of the built-in
// controller's name, else of the source's own (see eventSourceEventName).
[[nodiscard]] std::optional<std::string_view> eventSourceTokenName(std::string_view name) noexcept
{
    constexpr std::string_view controllerPrefix = "MIDIController";
    if (!name.starts_with(controllerPrefix))
        return name;
    const auto digits = name.substr(controllerPrefix.size());
    if (digits.empty() || digits.size() > 3)
        return std::nullopt;
    unsigned number = 0;
    for (const auto digit : digits)
    {
        if (!isAsciiDigit(digit))
            return std::nullopt;
        number = number * 10 + static_cast<unsigned>(digit - '0');
    }
    if (number > 127)
        return std::nullopt;
    return std::string_view{controllerEventNames.text[number].data(), controllerEventNames.size[number]};
}

class Lexer
{
public:
    Lexer(
        std::string_view source,
        ExpressionLanguageVersion languageVersion)
        : source_(source), languageVersion_(languageVersion)
    {
    }

    [[nodiscard]] std::size_t offset() const noexcept
    {
        return offset_;
    }

    Token next() noexcept
    {
        skipWhitespace();
        if (offset_ >= source_.size())
            return {TokenKind::End, {}, {offset_, 0}};

        const auto start = offset_;
        const auto value = source_[offset_];
        if (isAsciiDigit(value)
            || (value == '.' && offset_ + 1 < source_.size()
                && isAsciiDigit(source_[offset_ + 1])))
        {
            return scanNumber();
        }

        if (usesValMessages(languageVersion_))
        {
            if (auto token = nextValToken(start))
                return *token;
        }

        if (supportsCheckedRemainder(languageVersion_)
            && source_.substr(offset_, 4) == "rem:")
        {
            offset_ += 4;
            return {TokenKind::Remainder,
                    source_.substr(start, 4),
                    {start, 4}};
        }

        if (supportsTimedExpressions(languageVersion_)
            && source_.substr(offset_, 5) == "ramp:")
        {
            offset_ += 5;
            return {TokenKind::Ramp,
                    source_.substr(start, 5),
                    {start, 5}};
        }

        if (isIdentifierStart(value))
            return scanIdentifier(TokenKind::Identifier);

        if (value == '!')
        {
            ++offset_;
            if (isCompiledExpressionLanguage(languageVersion_)
                && offset_ < source_.size() && source_[offset_] == '=')
            {
                ++offset_;
                return {TokenKind::NotEqual,
                        source_.substr(start, 2),
                        rangeFrom(start, offset_)};
            }
            if (offset_ < source_.size()
                && isIdentifierStart(source_[offset_]))
            {
                const auto nameStart = offset_;
                while (offset_ < source_.size()
                    && isIdentifierContinue(source_[offset_]))
                {
                    ++offset_;
                }
                return {TokenKind::Event,
                        source_.substr(nameStart, offset_ - nameStart),
                        rangeFrom(start, offset_)};
            }
            return {TokenKind::InvalidEvent,
                    source_.substr(start, 1),
                    {start, 1}};
        }

        // An Event Source, `MIDIController07,
        // read as the Event Value the engine keeps for it (see
        // eventSourceEventName); its range is the backquote and the name.
        if (value == '`' && usesValMessages(languageVersion_))
        {
            ++offset_;
            const auto nameStart = offset_;
            while (offset_ < source_.size() && isIdentifierContinue(source_[offset_]))
                ++offset_;
            const auto name = source_.substr(nameStart, offset_ - nameStart);
            if (name == "MIDITimingClock")
                return {TokenKind::MidiTimingClockSource, name, rangeFrom(start, offset_)};
            const auto event = name.empty() || !isIdentifierStart(name.front())
                ? std::nullopt
                : eventSourceTokenName(name);
            if (!event.has_value())
                return {TokenKind::InvalidEventSource,
                        source_.substr(start, offset_ - start),
                        rangeFrom(start, offset_)};
            return {TokenKind::Event, *event, rangeFrom(start, offset_)};
        }

        if (value == '?' && supportsVoiceNumberSpecialization(languageVersion_))
        {
            ++offset_;
            while (offset_ < source_.size() && isIdentifierContinue(source_[offset_]))
                ++offset_;
            const auto text = source_.substr(start, offset_ - start);
            return {text == voiceNumberIdentifier ? TokenKind::Identifier : TokenKind::InvalidVoiceNumber,
                    text, rangeFrom(start, offset_)};
        }

        // A variable as it is typed into a Val field, ?freq beside
        // @freq: a letter, then letters and digits.
        if (value == '?' && usesValMessages(languageVersion_))
        {
            ++offset_;
            while (offset_ < source_.size() && isIdentifierContinue(source_[offset_]))
                ++offset_;
            const auto text = source_.substr(start, offset_ - start);
            return text.size() > 1 && isIdentifierStart(text[1]) && text != voiceNumberIdentifier
                ? Token{TokenKind::Variable, text.substr(1), rangeFrom(start, offset_)}
                : Token{TokenKind::InvalidVariable, text, rangeFrom(start, offset_)};
        }

        if (value == '@' && supportsVariableReferences(languageVersion_))
        {
            ++offset_;
            if (offset_ < source_.size()
                && isIdentifierStart(source_[offset_]))
            {
                const auto nameStart = offset_;
                while (offset_ < source_.size()
                    && isIdentifierContinue(source_[offset_]))
                {
                    ++offset_;
                }
                return {TokenKind::Variable,
                        source_.substr(nameStart, offset_ - nameStart),
                        rangeFrom(start, offset_)};
            }
            return {TokenKind::InvalidVariable,
                    source_.substr(start, 1),
                    {start, 1}};
        }

        ++offset_;
        switch (value)
        {
        case '+':
            return {TokenKind::Plus, source_.substr(start, 1), {start, 1}};
        case '-':
            return {TokenKind::Minus, source_.substr(start, 1), {start, 1}};
        case '*':
            return {TokenKind::Star, source_.substr(start, 1), {start, 1}};
        case '/':
            return {TokenKind::Slash, source_.substr(start, 1), {start, 1}};
        case '(':
            return {TokenKind::LeftParen,
                    source_.substr(start, 1),
                    {start, 1}};
        case ')':
            return {TokenKind::RightParen,
                    source_.substr(start, 1),
                    {start, 1}};
        case '[':
            if (isCompiledExpressionLanguage(languageVersion_))
                return {TokenKind::LeftBracket,
                        source_.substr(start, 1),
                        {start, 1}};
            break;
        case ']':
            if (isCompiledExpressionLanguage(languageVersion_))
                return {TokenKind::RightBracket,
                        source_.substr(start, 1),
                        {start, 1}};
            break;
        case ',':
            if (isCompiledExpressionLanguage(languageVersion_))
                return {TokenKind::Comma, source_.substr(start, 1), {start, 1}};
            break;
        case '<':
            if (!isCompiledExpressionLanguage(languageVersion_))
                break;
            if (usesValMessages(languageVersion_) && offset_ < source_.size()
                && (source_[offset_] == '~' || source_[offset_] == '+'))
            {
                const auto keeps = source_[offset_] == '~';
                ++offset_;
                return {keeps ? TokenKind::AssignKeep : TokenKind::AssignPop,
                        source_.substr(start, 2),
                        rangeFrom(start, offset_)};
            }
            if (offset_ < source_.size() && source_[offset_] == '=')
            {
                ++offset_;
                return {TokenKind::LessEqual,
                        source_.substr(start, 2),
                        rangeFrom(start, offset_)};
            }
            return {TokenKind::Less, source_.substr(start, 1), {start, 1}};
        case '>':
            if (!isCompiledExpressionLanguage(languageVersion_))
                break;
            if (offset_ < source_.size() && source_[offset_] == '=')
            {
                ++offset_;
                return {TokenKind::GreaterEqual,
                        source_.substr(start, 2),
                        rangeFrom(start, offset_)};
            }
            return {TokenKind::Greater,
                    source_.substr(start, 1),
                    {start, 1}};
        case '=':
            if (isCompiledExpressionLanguage(languageVersion_)
                && offset_ < source_.size() && source_[offset_] == '=')
            {
                ++offset_;
                return {TokenKind::EqualEqual,
                        source_.substr(start, 2),
                        rangeFrom(start, offset_)};
            }
            break;
        default:
            break;
        }
        return {TokenKind::InvalidCharacter,
                source_.substr(start, 1),
                {start, 1}};
    }

private:
    Token scanNumber() noexcept
    {
        const auto start = offset_;
        while (offset_ < source_.size()
            && isAsciiDigit(source_[offset_]))
        {
            ++offset_;
        }
        // A Val time code, hh:mm:ss.ff, which SMPTE follows.
        if (usesValMessages(languageVersion_)
            && offset_ + 1 < source_.size() && source_[offset_] == ':'
            && isAsciiDigit(source_[offset_ + 1]))
        {
            for (int field = 0; field < 2; ++field)
            {
                if (offset_ + 1 >= source_.size() || source_[offset_] != ':'
                    || !isAsciiDigit(source_[offset_ + 1]))
                {
                    break;
                }
                ++offset_;
                while (offset_ < source_.size() && isAsciiDigit(source_[offset_]))
                    ++offset_;
            }
            if (offset_ + 1 < source_.size() && source_[offset_] == '.'
                && isAsciiDigit(source_[offset_ + 1]))
            {
                ++offset_;
                while (offset_ < source_.size() && isAsciiDigit(source_[offset_]))
                    ++offset_;
            }
            return {TokenKind::Number,
                    source_.substr(start, offset_ - start),
                    rangeFrom(start, offset_)};
        }
        // A Val integer in another radix, as 16rFFEF or 2r1011.
        if (usesValMessages(languageVersion_)
            && offset_ + 1 < source_.size() && source_[offset_] == 'r'
            && std::isalnum(static_cast<unsigned char>(source_[offset_ + 1])) != 0)
        {
            ++offset_;
            while (offset_ < source_.size()
                && std::isalnum(static_cast<unsigned char>(source_[offset_])) != 0)
            {
                ++offset_;
            }
            return {TokenKind::Number,
                    source_.substr(start, offset_ - start),
                    rangeFrom(start, offset_)};
        }
        if (offset_ < source_.size() && source_[offset_] == '.')
        {
            ++offset_;
            while (offset_ < source_.size()
                && isAsciiDigit(source_[offset_]))
            {
                ++offset_;
            }
        }
        if (offset_ < source_.size()
            && (source_[offset_] == 'e' || source_[offset_] == 'E'))
        {
            const auto exponentStart = offset_;
            ++offset_;
            if (offset_ < source_.size()
                && (source_[offset_] == '+' || source_[offset_] == '-'))
            {
                ++offset_;
            }
            const auto digitsStart = offset_;
            while (offset_ < source_.size()
                && isAsciiDigit(source_[offset_]))
            {
                ++offset_;
            }
            if (digitsStart == offset_)
                offset_ = exponentStart;
        }
        // A Val double, 8.1758d or 1.14d10, as Smalltalk writes one
        //; a d followed by a letter begins a word.
        if (usesValMessages(languageVersion_)
            && offset_ < source_.size() && source_[offset_] == 'd')
        {
            auto end = offset_ + 1;
            if (end + 1 < source_.size() && source_[end] == '-' && isAsciiDigit(source_[end + 1]))
                ++end;
            while (end < source_.size() && isAsciiDigit(source_[end]))
                ++end;
            if (end == source_.size()
                || (std::isalnum(static_cast<unsigned char>(source_[end])) == 0 && source_[end] != '_'))
            {
                offset_ = end;
            }
        }
        return {TokenKind::Number,
                source_.substr(start, offset_ - start),
                rangeFrom(start, offset_)};
    }

    Token scanIdentifier(TokenKind kind) noexcept
    {
        const auto start = offset_;
        while (offset_ < source_.size()
            && isIdentifierContinue(source_[offset_]))
        {
            ++offset_;
        }
        return {kind,
                source_.substr(start, offset_ - start),
                rangeFrom(start, offset_)};
    }

    // Val's keywords (a name ending in a colon), its two-character
    // operators ** // \\, literal arrays #( ... ) and points {x @ y}.
    std::optional<Token> nextValToken(std::size_t start) noexcept
    {
        const auto value = source_[offset_];
        if (isIdentifierStart(value))
        {
            auto token = scanIdentifier(TokenKind::Identifier);
            if (offset_ < source_.size() && source_[offset_] == ':')
            {
                ++offset_;
                token.kind = TokenKind::Keyword;
                token.text = source_.substr(start, offset_ - start);
                token.range = rangeFrom(start, offset_);
            }
            return token;
        }
        const auto next = offset_ + 1 < source_.size()
            ? source_[offset_ + 1]
            : '\0';
        const auto token = [&](TokenKind kind, std::size_t length) {
            offset_ += length;
            return Token{kind, source_.substr(start, length), {start, length}};
        };
        if (value == '*' && next == '*')
            return token(TokenKind::Power, 2);
        if (value == '/' && next == '/')
            return token(TokenKind::IntegerDivide, 2);
        if (value == '\\' && next == '\\')
            return token(TokenKind::Modulo, 2);
        // Smalltalk's = and ~= compare numbers.
        if (value == '=' && next != '=')
            return token(TokenKind::EqualEqual, 1);
        if (value == '~' && next == '=')
            return token(TokenKind::NotEqual, 2);
        if (value == '#' && next == '(')
            return token(TokenKind::ArrayStart, 2);
        // The Array a braced collect: program writes out, each element of
        // which runs on every tick (see withCollectedArrays).
        if (value == '#' && next == eagerArrayMark && offset_ + 2 < source_.size() && source_[offset_ + 2] == '(')
            return token(TokenKind::ArrayStart, 3);
        if (value == '{')
            return token(TokenKind::LeftBrace, 1);
        if (value == '}')
            return token(TokenKind::RightBrace, 1);
        if (value == '@' && !isIdentifierStart(next))
            return token(TokenKind::At, 1);
        return std::nullopt;
    }

    void skipWhitespace() noexcept
    {
        while (offset_ < source_.size())
        {
            if (isAsciiWhitespace(source_[offset_]))
            {
                ++offset_;
                continue;
            }
            // A Val field's comment, in double quotes; an unclosed one
            // stays an error.
            if (usesValMessages(languageVersion_) && source_[offset_] == '"')
            {
                const auto close = source_.find('"', offset_ + 1);
                if (close == std::string_view::npos)
                    return;
                offset_ = close + 1;
                continue;
            }
            return;
        }
    }

    std::string_view source_;
    ExpressionLanguageVersion languageVersion_ =
        ExpressionLanguageVersion::LegacyV1;
    std::size_t offset_ = 0;
};

// An Event Value written with its '!', not an Event Source (`name) read as
// one.
[[nodiscard]] bool isEventValueToken(std::string_view source, const Token& token) noexcept
{
    return token.kind == TokenKind::Event && token.range.offset < source.size()
        && source[token.range.offset] == '!';
}

[[nodiscard]] std::optional<Unit> parseUnit(std::string_view text) noexcept
{
    if (text == "hz")
        return Unit::Hertz;
    // 0 dB is the usual spelling; db also reads.
    if (text == "db" || text == "dB")
        return Unit::Decibels;
    if (text == "ms")
        return Unit::Milliseconds;
    if (text == "usec")
        return Unit::Microseconds;
    if (text == "s")
        return Unit::Seconds;
    if (text == "m")
        return Unit::Minutes;
    if (text == "h")
        return Unit::Hours;
    if (text == "days")
        return Unit::Days;
    if (text == "nn")
        return Unit::NoteNumber;
    if (text == "beats")
        return Unit::Beats;
    if (text == "samples" || text == "samp")
        return Unit::Samples;
    return std::nullopt;
}

// A Val unit message: the portable units plus bpm, and beat as well as
// beats.
[[nodiscard]] std::optional<Unit> valUnit(std::string_view text) noexcept
{
    if (text == "bpm")
        return Unit::BeatsPerMinute;
    if (text == "beat")
        return Unit::Beats;
    // 1 day as well as 2 days.
    if (text == "day")
        return Unit::Days;
    return parseUnit(text);
}

// Val unary messages other than units and negated. log is base 10 here;
// the log() function keeps its natural logarithm.
[[nodiscard]] std::optional<detail::FunctionOp> valUnaryMessage(
    std::string_view text) noexcept
{
    constexpr std::array<std::pair<std::string_view, detail::FunctionOp>, 29>
        messages{{
            {"abs", detail::FunctionOp::Abs},
            {"sqrt", detail::FunctionOp::Sqrt},
            {"exp", detail::FunctionOp::Exp},
            {"log", detail::FunctionOp::Log10},
            {"twoLog", detail::FunctionOp::Log2},
            {"twoExp", detail::FunctionOp::Exp2},
            {"squared", detail::FunctionOp::Squared},
            {"inverse", detail::FunctionOp::Reciprocal},
            {"sign", detail::FunctionOp::Sign},
            {"truncated", detail::FunctionOp::Truncate},
            {"rounded", detail::FunctionOp::Round},
            {"clipTo01", detail::FunctionOp::ClipTo01},
            {"wrapTo02", detail::FunctionOp::WrapTo02},
            {"cos", detail::FunctionOp::Cos},
            {"sin", detail::FunctionOp::Sin},
            {"normCos", detail::FunctionOp::NormCos},
            {"normSin", detail::FunctionOp::NormSin},
            {"asLogicValue", detail::FunctionOp::LogicValue},
            {"removeUnits", detail::FunctionOp::RemoveUnits},
            // Smalltalk's messages to numbers.
            {"reciprocal", detail::FunctionOp::Reciprocal},
            {"asInteger", detail::FunctionOp::Truncate},
            {"floor", detail::FunctionOp::Floor},
            {"ceiling", detail::FunctionOp::Ceiling},
            {"ln", detail::FunctionOp::Log},
            {"arcTan", detail::FunctionOp::ArcTan},
            {"arcSin", detail::FunctionOp::ArcSin},
            {"arcCos", detail::FunctionOp::ArcCos},
            {"tan", detail::FunctionOp::Tan},
            {"sinc", detail::FunctionOp::Sinc},
        }};
    const auto found = std::ranges::find(
        messages,
        text,
        &std::pair<std::string_view, detail::FunctionOp>::first);
    if (found == messages.end())
        return std::nullopt;
    return found->second;
}

// The TimeFunction variant that counts seconds since it started, for
// !LocalTime, and the one counting seconds since the whole Sound
// started, for !RealTime and !Time; the others select ramp shapes.
inline constexpr int localTimeVariant = 8;
inline constexpr int wholeSoundTimeVariant = 9;
// !Time, which TimeControls and TimeStoppers slow or stop where !RealTime
// runs on.
inline constexpr int controlledWholeSoundTimeVariant = 10;

// Val unary messages that are functions of time or of a trigger.
// A default duration is given in seconds; variant selects the ramp shape.
struct ValTimedMessage
{
    detail::FunctionOp function;
    double variant = 0.0;
    std::optional<double> defaultSeconds;
};

[[nodiscard]] std::optional<ValTimedMessage> valTimedMessage(
    std::string_view text) noexcept
{
    using detail::FunctionOp;
    if (text == "ramp")
        return ValTimedMessage{FunctionOp::Ramp, 0.0, 1.0};
    if (text == "fullRamp")
        return ValTimedMessage{FunctionOp::TimeFunction, 1.0, 1.0};
    if (text == "triangle")
        return ValTimedMessage{FunctionOp::TimeFunction, 2.0, 1.0};
    if (text == "repeatingRamp")
        return ValTimedMessage{FunctionOp::TimeFunction, 4.0, 1.0};
    if (text == "repeatingFullRamp")
        return ValTimedMessage{FunctionOp::TimeFunction, 5.0, 1.0};
    if (text == "repeatingTriangle")
        return ValTimedMessage{FunctionOp::TimeFunction, 6.0, 1.0};
    if (text == "smoothed")
        return ValTimedMessage{FunctionOp::Smooth, 0.0, 0.1};
    // The variant names the distribution: 0 uniform, 1 normal, 2
    // exponential.
    if (text == "random")
        return ValTimedMessage{FunctionOp::RandomInterval, 0.0, std::nullopt};
    if (text == "normal")
        return ValTimedMessage{FunctionOp::RandomInterval, 1.0, std::nullopt};
    if (text == "randExp")
        return ValTimedMessage{FunctionOp::RandomInterval, 2.0, std::nullopt};
    if (text == "nextRandom")
        return ValTimedMessage{FunctionOp::NextRandom, 0.0, std::nullopt};
    if (text == "nextNormal")
        return ValTimedMessage{FunctionOp::NextRandom, 1.0, std::nullopt};
    if (text == "nextRandExp")
        return ValTimedMessage{FunctionOp::NextRandom, 2.0, std::nullopt};
    return std::nullopt;
}

// Pitch names, as letters or solfege syllables, in half steps above c.
[[nodiscard]] std::optional<double> parsePitchName(std::string_view text) noexcept
{
    // The solfege names are spelled do, re, mi, fa, so, la, ti; sol and
    // si are the common alternatives.
    constexpr std::array<std::pair<std::string_view, double>, 16> names{{
        {"c", 0.0}, {"do", 0.0}, {"d", 2.0}, {"re", 2.0}, {"e", 4.0},
        {"mi", 4.0}, {"f", 5.0}, {"fa", 5.0}, {"g", 7.0}, {"so", 7.0},
        {"sol", 7.0}, {"a", 9.0}, {"la", 9.0}, {"b", 11.0}, {"si", 11.0},
        {"ti", 11.0},
    }};
    const auto found = std::ranges::find(
        names, text, &std::pair<std::string_view, double>::first);
    if (found == names.end())
        return std::nullopt;
    return found->second;
}

[[nodiscard]] double parseDouble(std::string_view text, bool& ok) noexcept
{
    // An integer in another radix, its numerals past nine letters of
    // either case.
    if (const auto r = text.find('r'); r != std::string_view::npos && r > 0)
    {
        int radix = 0;
        const auto [radixEnd, radixError] = std::from_chars(text.data(), text.data() + r, radix);
        ok = radixError == std::errc{} && radixEnd == text.data() + r && radix >= 2 && radix <= 36
            && r + 1 < text.size();
        auto value = 0.0;
        for (std::size_t index = r + 1; ok && index < text.size(); ++index)
        {
            const auto character = static_cast<unsigned char>(text[index]);
            const auto digit = std::isdigit(character) != 0 ? character - '0'
                : std::isalpha(character) != 0               ? std::toupper(character) - 'A' + 10
                                                               : 99;
            ok = digit < radix;
            value = value * radix + digit;
        }
        ok = ok && std::isfinite(value);
        return ok ? value : 0.0;
    }
    // A double's d marks its exponent, if any, as e would.
    std::array<char, 64> exponent{};
    if (const auto d = text.find('d'); d != std::string_view::npos)
    {
        if (text.size() > exponent.size())
        {
            ok = false;
            return 0.0;
        }
        std::ranges::copy(text, exponent.begin());
        exponent[d] = 'e';
        text = {exponent.data(), d + 1 < text.size() ? text.size() : d};
    }
    double value = 0.0;
    const auto* const begin = text.data();
    const auto* const end = begin + text.size();
    const auto [parsedEnd, error] = talas::fromChars(
        begin,
        end,
        value,
        std::chars_format::general);
    ok = error == std::errc{} && parsedEnd == end
        && std::isfinite(value);
    return ok ? value : 0.0;
}

[[nodiscard]] Diagnostic makeDiagnostic(
    Diagnostic::Code code,
    SourceRange range,
    std::string message)
{
    return {code, range, std::move(message)};
}

struct ParsedExpression
{
    std::optional<detail::ExpressionStorage> storage;
    std::vector<Diagnostic> diagnostics;
};

[[nodiscard]] std::optional<detail::FunctionOp> functionFor(
    std::string_view name) noexcept
{
    if (name == "abs")
        return detail::FunctionOp::Abs;
    if (name == "sqrt")
        return detail::FunctionOp::Sqrt;
    if (name == "log")
        return detail::FunctionOp::Log;
    if (name == "exp")
        return detail::FunctionOp::Exp;
    if (name == "clamp")
        return detail::FunctionOp::Clamp;
    if (name == "map")
        return detail::FunctionOp::Map;
    if (name == "if")
        return detail::FunctionOp::If;
    if (name == "previous")
        return detail::FunctionOp::Previous;
    if (name == "talasRecurrence")
        return detail::FunctionOp::Recurrence;
    if (name == "talasEventVariable")
        return detail::FunctionOp::EventVariableLoad;
    if (name == "talasShared")
        return detail::FunctionOp::Shared;
    if (name == "accumulate")
        return detail::FunctionOp::Accumulate;
    if (name == "sampleHold")
        return detail::FunctionOp::SampleHold;
    return std::nullopt;
}

[[nodiscard]] bool isStatefulFunction(
    detail::FunctionOp function) noexcept
{
    return function == detail::FunctionOp::Previous
        || function == detail::FunctionOp::Accumulate
        || function == detail::FunctionOp::SampleHold
        || function == detail::FunctionOp::Ramp
        || function == detail::FunctionOp::TimeFunction
        || function == detail::FunctionOp::Metronome
        || function == detail::FunctionOp::RandomInterval
        || function == detail::FunctionOp::NextRandom
        || function == detail::FunctionOp::Smooth
        || function == detail::FunctionOp::CountTriggers
        || function == detail::FunctionOp::TriggerEvery
        || function == detail::FunctionOp::Toggle
        || function == detail::FunctionOp::GateWhen
        || function == detail::FunctionOp::TriggerDuration
        || function == detail::FunctionOp::ChangedReset
        || function == detail::FunctionOp::SustainBy
        || function == detail::FunctionOp::NextRandomSeeded
        || function == detail::FunctionOp::Chaotic
        || function == detail::FunctionOp::SwarmFollow
        || function == detail::FunctionOp::TrackHold
        || function == detail::FunctionOp::Initially
        || function == detail::FunctionOp::Recurrence
        || function == detail::FunctionOp::EventVariableLoad
        || function == detail::FunctionOp::EventVariableStore
        || function == detail::FunctionOp::MidiTimingClock;
}

[[nodiscard]] std::optional<detail::FunctionOp> availableFunctionFor(
    std::string_view name,
    ExpressionLanguageVersion languageVersion) noexcept
{
    const auto function = functionFor(name);
    if (function.has_value()
        && isStatefulFunction(*function)
        && !supportsStatefulExpressions(languageVersion))
    {
        return std::nullopt;
    }
    return function;
}

[[nodiscard]] std::uint8_t functionArity(
    detail::FunctionOp function) noexcept
{
    switch (function)
    {
    case detail::FunctionOp::Abs:
    case detail::FunctionOp::Sqrt:
    case detail::FunctionOp::Log:
    case detail::FunctionOp::Exp:
    case detail::FunctionOp::Log10:
    case detail::FunctionOp::Log2:
    case detail::FunctionOp::Exp2:
    case detail::FunctionOp::Squared:
    case detail::FunctionOp::Reciprocal:
    case detail::FunctionOp::Sign:
    case detail::FunctionOp::Truncate:
    case detail::FunctionOp::Round:
    case detail::FunctionOp::ClipTo01:
    case detail::FunctionOp::WrapTo02:
    case detail::FunctionOp::Cos:
    case detail::FunctionOp::Sin:
    case detail::FunctionOp::NormCos:
    case detail::FunctionOp::NormSin:
    case detail::FunctionOp::LogicValue:
    case detail::FunctionOp::RemoveUnits:
    case detail::FunctionOp::ArrayIndex:
    case detail::FunctionOp::RandomInterval:
    case detail::FunctionOp::NextRandom:
    case detail::FunctionOp::Floor:
    case detail::FunctionOp::Ceiling:
    case detail::FunctionOp::RoundHalfUp:
    case detail::FunctionOp::LinearUnitlessSOSPitch:
    case detail::FunctionOp::ArcTan:
    case detail::FunctionOp::ArcSin:
    case detail::FunctionOp::ArcCos:
    case detail::FunctionOp::Tan:
    case detail::FunctionOp::Sinc:
        return 1;
    case detail::FunctionOp::Previous:
    case detail::FunctionOp::Ramp:
    case detail::FunctionOp::Min:
    case detail::FunctionOp::Max:
    case detail::FunctionOp::RoundTo:
    case detail::FunctionOp::TimeFunction:
    case detail::FunctionOp::Smooth:
    case detail::FunctionOp::TriggerEvery:
    case detail::FunctionOp::Toggle:
    case detail::FunctionOp::GateWhen:
    case detail::FunctionOp::TriggerDuration:
    case detail::FunctionOp::ChangedReset:
    case detail::FunctionOp::SustainBy:
    case detail::FunctionOp::NextRandomSeeded:
    case detail::FunctionOp::TrackHold:
    case detail::FunctionOp::TruncateTo:
    case detail::FunctionOp::CeilingTo:
    case detail::FunctionOp::MaxAbs:
    case detail::FunctionOp::MinAbs:
    case detail::FunctionOp::LogBase:
    case detail::FunctionOp::FloorLog:
        return 2;
    case detail::FunctionOp::Metronome:
    case detail::FunctionOp::CountTriggers:
        return 3;
    case detail::FunctionOp::Chaotic:
        return 4;
    case detail::FunctionOp::Clamp:
    case detail::FunctionOp::If:
    case detail::FunctionOp::Accumulate:
    case detail::FunctionOp::SampleHold:
    case detail::FunctionOp::Between:
        return 3;
    case detail::FunctionOp::Map:
    case detail::FunctionOp::Initially:
    case detail::FunctionOp::SwarmFollow:
        return 5;
    case detail::FunctionOp::Recurrence:
    case detail::FunctionOp::UpdateInternalState:
    case detail::FunctionOp::EveryMillisecond:
    case detail::FunctionOp::MidiTimingClock:
        return 1;
    case detail::FunctionOp::EventVariableLoad:
    case detail::FunctionOp::EventVariableStore:
    case detail::FunctionOp::Shared:
    case detail::FunctionOp::Sequence:
        return 2;
    case detail::FunctionOp::Nothing:
        return 0;
    }
    return 0;
}

class Parser
{
public:
    Parser(
        std::string_view source,
        ExpressionLanguageVersion languageVersion,
        const ExpressionDefinitionLibrary* definitions = nullptr,
        const std::vector<ExpressionDefinitionParameter>* formals = nullptr)
        : source_(source),
          lexer_(source, languageVersion),
          languageVersion_(languageVersion),
          definitions_(definitions),
          formals_(formals)
    {
        storage_.languageVersion = languageVersion;
    }

    // Where each bpm that a plain number multiplies or divides before
    // removeUnits ends, recorded as the expression parses.
    void recordBpmFactors(std::vector<std::size_t>* ends) noexcept
    {
        bpmFactorEnds_ = ends;
    }

    // Where each bpm: or bpm:dutyCycle: message lies, recorded as the
    // expression parses.
    void recordMetronomes(std::vector<SourceRange>* ranges) noexcept
    {
        metronomeRanges_ = ranges;
    }

    void recordSignSelections(std::vector<SourceRange>* ranges) noexcept
    {
        signSelectionRanges_ = ranges;
    }

    // Whether !RealTime and !Time read the whole Sound's clock.
    void readWholeSoundClocks(bool clocks) noexcept
    {
        wholeSoundClocks_ = clocks;
    }

    ParsedExpression parse()
    {
        if (isCompiledExpressionLanguage(languageVersion_)
            && source_.size() > ExpressionLimits::maximumSourceBytes)
        {
            failBudget(
                {ExpressionLimits::maximumSourceBytes, 0},
                "Expression source exceeds the fixed byte budget");
            return finish(std::nullopt);
        }

        advance();
        if (current_.kind == TokenKind::End)
        {
            addDiagnostic(
                Diagnostic::Code::EmptyExpression,
                {0, 0},
                "Expression is empty");
            return finish(std::nullopt);
        }

        auto root = usesValMessages(languageVersion_) ? parseValSequence(0) : parseExpression(0, 0);
        if (root.has_value() && current_.kind != TokenKind::End)
            addTrailingDiagnostic();
        if (root.has_value())
            storage_.root = *root;
        return finish(root);
    }

private:
    std::optional<NodeId> parseExpression(
        int minimumBindingPower,
        std::size_t depth)
    {
        if (usesValMessages(languageVersion_))
            return parseKeywordMessage(depth);
        if (!admitDepth(depth))
            return std::nullopt;
        auto left = parsePrefix(depth);
        if (!left.has_value())
            return std::nullopt;

        while (!budgetExceeded_)
        {
            if (current_.kind == TokenKind::Identifier
                && supportsPostfixUnits(languageVersion_))
            {
                const auto unit = parseUnit(current_.text);
                if (!unit)
                {
                    addDiagnostic(Diagnostic::Code::UnknownUnit,
                        current_.range, "Unknown expression unit");
                    advance();
                    return left;
                }
                if (*unit == Unit::Beats && fieldBeatSeconds().has_value())
                {
                    left = applyFieldBeats(*left, joinRanges(storage_.nodes[*left].range, current_.range));
                    advance();
                    if (!left) return std::nullopt;
                    continue;
                }
                detail::ExpressionNode node;
                node.kind = detail::NodeKind::UnitApplication;
                node.left = *left;
                node.unit = *unit;
                node.range = joinRanges(storage_.nodes[*left].range, current_.range);
                advance();
                left = addNode(std::move(node));
                if (!left) return std::nullopt;
                continue;
            }
            if (current_.kind == TokenKind::Ramp
                && minimumBindingPower <= 3)
            {
                const auto message = current_;
                if (minimumBindingPower != 0)
                {
                    addDiagnostic(
                        Diagnostic::Code::UnexpectedToken,
                        message.range,
                        "Parenthesize this ramp: receiver; Val sends "
                        "keyword messages after binary operators");
                    return left;
                }
                advance();
                const auto duration = parseExpression(3, depth + 1);
                if (!duration.has_value())
                    return left;
                if (comparisonOpFor(current_.kind).has_value()
                    || binaryOpFor(current_.kind).has_value())
                {
                    addDiagnostic(
                        Diagnostic::Code::UnexpectedToken,
                        current_.range,
                        "Parenthesize this ramp: duration; Val sends "
                        "keyword messages after binary operators");
                    return left;
                }
                detail::ExpressionNode node;
                node.kind = detail::NodeKind::Call;
                node.functionOp = detail::FunctionOp::Ramp;
                node.arguments[0] = *left;
                node.arguments[1] = *duration;
                node.argumentCount = 2;
                node.range = joinRanges(
                    storage_.nodes[*left].range,
                    storage_.nodes[*duration].range);
                node.range = joinRanges(node.range, message.range);
                left = addNode(std::move(node));
                if (!left.has_value())
                    return std::nullopt;
                continue;
            }
            const auto comparison = comparisonOpFor(current_.kind);
            const auto binary = binaryOpFor(current_.kind);
            if (!comparison.has_value() && !binary.has_value())
                break;
            if (comparison.has_value()
                && !isCompiledExpressionLanguage(languageVersion_))
            {
                break;
            }

            const auto precedence = bindingPower(current_.kind);
            if (precedence < minimumBindingPower)
                break;

            const auto operatorToken = current_;
            advance();
            auto right = parseExpression(precedence + 1, depth + 1);
            if (!right.has_value())
                return left;

            detail::ExpressionNode node;
            node.kind = comparison.has_value()
                ? detail::NodeKind::Comparison
                : detail::NodeKind::Binary;
            if (comparison.has_value())
                node.comparisonOp = *comparison;
            else
                node.binaryOp = *binary;
            node.left = *left;
            node.right = *right;
            node.range = joinRanges(
                storage_.nodes[*left].range,
                storage_.nodes[*right].range);
            node.range = joinRanges(node.range, operatorToken.range);
            left = addNode(std::move(node));
            if (!left.has_value())
                return std::nullopt;
        }
        return left;
    }

    // A Val literal array: numbers, !Event Values or {expressions}
    // for of:, or {x @ y} points for into:.
    struct LiteralArray
    {
        std::vector<NodeId> values;
        std::vector<NodeId> xs;
        std::vector<NodeId> ys;
        SourceRange range;
        // Each element runs on every tick, as a braced collect: program's.
        bool eager = false;
    };

    // Val sends keyword messages last, so the receiver and each
    // argument are binary expressions.
    // Expressions in parentheses separated by commas are
    // evaluated in turn.
    std::optional<NodeId> parseValSequence(std::size_t depth)
    {
        auto left = parseKeywordMessage(depth);
        while (left.has_value() && current_.kind == TokenKind::Comma && !budgetExceeded_)
        {
            advance();
            const auto right = parseKeywordMessage(depth + 1);
            if (!right.has_value())
                return std::nullopt;
            detail::ExpressionNode node;
            node.kind = detail::NodeKind::Call;
            node.functionOp = detail::FunctionOp::Sequence;
            node.arguments[0] = *left;
            node.arguments[1] = *right;
            node.argumentCount = 2;
            node.range = joinRanges(storage_.nodes[*left].range, storage_.nodes[*right].range);
            left = addNode(std::move(node));
        }
        return left;
    }

    std::optional<NodeId> parseKeywordMessage(std::size_t depth)
    {
        if (!admitDepth(depth))
            return std::nullopt;
        const auto receiver = parseBinaryMessages(depth);
        if (!receiver.has_value() || current_.kind != TokenKind::Keyword)
            return receiver;

        std::string selector;
        // Four for neg:zero:pos:initially:.
        std::array<NodeId, 4> arguments{};
        std::size_t argumentCount = 0;
        std::optional<LiteralArray> array;
        auto range = storage_.nodes[*receiver].range;
        while (!budgetExceeded_ && current_.kind == TokenKind::Keyword)
        {
            selector += current_.text;
            range = joinRanges(range, current_.range);
            advance();
            if (current_.kind == TokenKind::ArrayStart && !array.has_value()
                && argumentCount == 0)
            {
                array = parseLiteralArray(depth + 1);
                if (!array.has_value())
                    return std::nullopt;
                range = joinRanges(range, array->range);
                continue;
            }
            if (array.has_value() || argumentCount == arguments.size())
            {
                addDiagnostic(
                    Diagnostic::Code::InvalidArgumentCount,
                    range,
                    "Unknown Val message " + selector);
                return std::nullopt;
            }
            const auto argument = parseBinaryMessages(depth + 1);
            if (!argument.has_value())
                return std::nullopt;
            range = joinRanges(range, storage_.nodes[*argument].range);
            arguments[argumentCount++] = *argument;
        }
        return keywordMessage(
            selector,
            *receiver,
            std::span<const NodeId>{arguments}.first(argumentCount),
            array,
            range);
    }

    // Val binary operators all bind alike, from left to right.
    std::optional<NodeId> parseBinaryMessages(std::size_t depth)
    {
        auto left = parseUnaryMessages(depth);
        while (left.has_value() && !budgetExceeded_)
        {
            // AnEventVariable <~ anEventExpression assigns
            // it and keeps the value; <+ assigns it and leaves nothing.
            if (current_.kind == TokenKind::AssignKeep || current_.kind == TokenKind::AssignPop)
            {
                if (storage_.nodes[*left].kind != detail::NodeKind::Call
                    || storage_.nodes[*left].functionOp != detail::FunctionOp::EventVariableLoad)
                {
                    addDiagnostic(
                        Diagnostic::Code::InvalidArgument,
                        current_.range,
                        "Only an EventVariable can be assigned with <~ or <+");
                    return std::nullopt;
                }
                const auto assignment = current_;
                advance();
                const auto value = parseUnaryMessages(depth + 1);
                if (!value.has_value())
                    return std::nullopt;
                detail::ExpressionNode node;
                node.kind = detail::NodeKind::Call;
                node.functionOp = detail::FunctionOp::EventVariableStore;
                node.arguments[0] = *left;
                node.arguments[1] = *value;
                node.argumentCount = 2;
                node.number = assignment.kind == TokenKind::AssignKeep ? 1.0 : 0.0;
                node.range = joinRanges(
                    joinRanges(storage_.nodes[*left].range, assignment.range),
                    storage_.nodes[*value].range);
                left = addNode(std::move(node));
                continue;
            }
            const auto comparison = comparisonOpFor(current_.kind);
            const auto binary = current_.kind == TokenKind::Power
                ? std::optional{BinaryOp::Power}
                : current_.kind == TokenKind::IntegerDivide
                    ? std::optional{BinaryOp::IntegerDivide}
                    : current_.kind == TokenKind::Modulo
                        ? std::optional{BinaryOp::Modulo}
                        : current_.kind == TokenKind::Remainder
                            ? std::nullopt
                            : binaryOpFor(current_.kind);
            if (!comparison.has_value() && !binary.has_value())
                break;
            const auto operatorToken = current_;
            advance();
            const auto right = parseUnaryMessages(depth + 1);
            if (!right.has_value())
                return std::nullopt;
            detail::ExpressionNode node;
            node.kind = comparison.has_value()
                ? detail::NodeKind::Comparison
                : detail::NodeKind::Binary;
            if (comparison.has_value())
                node.comparisonOp = *comparison;
            else
                node.binaryOp = *binary;
            node.left = *left;
            node.right = *right;
            node.range = joinRanges(
                joinRanges(storage_.nodes[*left].range, operatorToken.range),
                storage_.nodes[*right].range);
            left = addNode(std::move(node));
        }
        return left;
    }

    // Val unary messages, including units such as hz or nn, bind
    // first and cascade from left to right.
    // Format 156: the seconds a beat written in a field lasts in a project keeping a tempo for them; nothing where a
    // beat follows the tempo the Sound plays to.
    [[nodiscard]] std::optional<double> fieldBeatSeconds() const noexcept
    {
        if (definitions_ == nullptr || !definitions_->fieldBeatTempoBpm.has_value()
            || !(*definitions_->fieldBeatTempoBpm > 0.0))
        {
            return std::nullopt;
        }
        return 60.0 / *definitions_->fieldBeatTempoBpm;
    }

    // A receiver in beats at the project's tempo for fields: a number
    // becomes its seconds, and anything else is multiplied by one beat's
    // seconds, as a beat converts a plain number to a time.
    std::optional<NodeId> applyFieldBeats(NodeId receiver, SourceRange range)
    {
        const auto seconds = fieldBeatSeconds().value_or(0.0);
        auto& target = storage_.nodes[receiver];
        if (target.kind == detail::NodeKind::Literal && target.unit == Unit::None)
        {
            target.number *= seconds;
            target.unit = Unit::Seconds;
            target.range = range;
            return receiver;
        }
        detail::ExpressionNode beat;
        beat.kind = detail::NodeKind::Literal;
        beat.number = seconds;
        beat.unit = Unit::Seconds;
        beat.range = range;
        const auto beatId = addNode(std::move(beat));
        if (!beatId.has_value())
            return std::nullopt;
        detail::ExpressionNode product;
        product.kind = detail::NodeKind::Binary;
        product.binaryOp = BinaryOp::Multiply;
        product.left = receiver;
        product.right = *beatId;
        product.range = range;
        return addNode(std::move(product));
    }

    // Smalltalk's unary messages to numbers that Val spells with its
    // own operations: nothing when the
    // selector is not one of them, the composed node otherwise.
    std::optional<std::optional<NodeId>> smalltalkNumberMessage(
        std::string_view selector,
        NodeId receiver,
        SourceRange range)
    {
        const auto literal = [&](double value) {
            detail::ExpressionNode node;
            node.kind = detail::NodeKind::Literal;
            node.number = value;
            node.range = range;
            return addNode(std::move(node));
        };
        const auto call = [&](detail::FunctionOp function,
                              std::optional<NodeId> argument) -> std::optional<NodeId> {
            if (!argument.has_value())
                return std::nullopt;
            detail::ExpressionNode node;
            node.kind = detail::NodeKind::Call;
            node.functionOp = function;
            node.arguments[0] = *argument;
            node.argumentCount = 1;
            node.range = range;
            return addNode(std::move(node));
        };
        const auto binary = [&](BinaryOp operation, std::optional<NodeId> left,
                                std::optional<NodeId> right) -> std::optional<NodeId> {
            if (!left.has_value() || !right.has_value())
                return std::nullopt;
            detail::ExpressionNode node;
            node.kind = detail::NodeKind::Binary;
            node.binaryOp = operation;
            node.left = *left;
            node.right = *right;
            node.range = range;
            return addNode(std::move(node));
        };
        const auto comparison = [&](detail::ComparisonOp operation, std::optional<NodeId> left,
                                    std::optional<NodeId> right) -> std::optional<NodeId> {
            if (!left.has_value() || !right.has_value())
                return std::nullopt;
            detail::ExpressionNode node;
            node.kind = detail::NodeKind::Comparison;
            node.comparisonOp = operation;
            node.left = *left;
            node.right = *right;
            node.range = range;
            return addNode(std::move(node));
        };
        if (selector == "asFloat" || selector == "asDouble")
            return std::optional<NodeId>{receiver};
        if (selector == "degreesToRadians")
            return binary(BinaryOp::Multiply, receiver, literal(std::numbers::pi / 180.0));
        if (selector == "radiansToDegrees")
            return binary(BinaryOp::Multiply, receiver, literal(180.0 / std::numbers::pi));
        // A trigger once the MIDI time code reaches the receiver's time
        //.
        if (selector == "triggerAtTimeCode")
        {
            return comparison(detail::ComparisonOp::GreaterEqual,
                midiTimeCodeReference(range), receiver);
        }
        if (selector == "negative")
            return comparison(detail::ComparisonOp::Less, receiver, literal(0.0));
        if (selector == "positive")
            return comparison(detail::ComparisonOp::GreaterEqual, receiver, literal(0.0));
        if (selector == "strictlyPositive")
            return comparison(detail::ComparisonOp::Greater, receiver, literal(0.0));
        if (selector == "even" || selector == "odd")
        {
            return comparison(detail::ComparisonOp::Equal,
                binary(BinaryOp::Modulo, receiver, literal(2.0)),
                literal(selector == "even" ? 0.0 : 1.0));
        }
        if (selector == "sqrtRounded")
            return call(detail::FunctionOp::Round, call(detail::FunctionOp::Sqrt, receiver));
        if (selector == "floorToTwoPower" || selector == "ceilingToTwoPower")
        {
            return call(detail::FunctionOp::Exp2,
                call(selector == "floorToTwoPower" ? detail::FunctionOp::Floor : detail::FunctionOp::Ceiling,
                    call(detail::FunctionOp::Log2, receiver)));
        }
        return std::nullopt;
    }

    std::optional<NodeId> parseUnaryMessages(std::size_t depth)
    {
        auto receiver = parseValPrimary(depth);
        while (receiver.has_value() && !budgetExceeded_
               && current_.kind == TokenKind::Identifier)
        {
            const auto message = current_;
            // A Sound pasted into the field, named with its channel.
            if (isPastedSoundChannel(message.text)
                && isPlainIdentifier(*receiver))
            {
                if (!nameSoundReference(*receiver, message.text, std::nullopt,
                        joinRanges(storage_.nodes[*receiver].range, message.range)))
                {
                    return std::nullopt;
                }
                advance();
                continue;
            }
            if (const auto composed = smalltalkNumberMessage(
                    message.text, *receiver,
                    joinRanges(storage_.nodes[*receiver].range, message.range)))
            {
                if (!composed->has_value())
                    return std::nullopt;
                advance();
                receiver = *composed;
                continue;
            }
            if (valUnit(message.text) == Unit::Beats && fieldBeatSeconds().has_value())
            {
                const auto beats = applyFieldBeats(
                    *receiver, joinRanges(storage_.nodes[*receiver].range, message.range));
                if (!beats.has_value())
                    return std::nullopt;
                advance();
                receiver = *beats;
                continue;
            }
            detail::ExpressionNode node;
            node.left = *receiver;
            node.range = joinRanges(storage_.nodes[*receiver].range, message.range);
            if (const auto unit = valUnit(message.text))
            {
                node.kind = detail::NodeKind::UnitApplication;
                node.unit = *unit;
            }
            else if (message.text == "negated")
            {
                node.kind = detail::NodeKind::Unary;
                node.unaryOp = UnaryOp::Negate;
            }
            else if (const auto timed = valTimedMessage(message.text))
            {
                node.kind = detail::NodeKind::Call;
                node.functionOp = timed->function;
                node.number = timed->variant;
                node.arguments[0] = *receiver;
                node.argumentCount = 1;
                if (timed->defaultSeconds.has_value())
                {
                    detail::ExpressionNode duration;
                    duration.kind = detail::NodeKind::Literal;
                    duration.number = *timed->defaultSeconds;
                    duration.unit = Unit::Seconds;
                    duration.range = message.range;
                    const auto durationId = addNode(std::move(duration));
                    if (!durationId.has_value())
                        return std::nullopt;
                    node.arguments[1] = *durationId;
                    node.argumentCount = 2;
                }
            }
            else if (message.text == "inSOSPitch" && definitions_ != nullptr
                     && definitions_->unitlessSOSPitch == UnitlessSOSPitch::Linear)
            {
                // Format 156: a project reading a number without units
                // linearly, as a fraction of 22050 hz, so 11025.0 inSOSPitch
                // is 0.5; hertz and note numbers keep the pitch scale.
                node.kind = detail::NodeKind::Call;
                node.functionOp = detail::FunctionOp::LinearUnitlessSOSPitch;
                node.arguments[0] = *receiver;
                node.argumentCount = 1;
            }
            else if (message.text == "inSOSPitch")
            {
                // A frequency or pitch as the spectral sources' pitch:
                // 22050 hz is 1 and each octave down takes 1/15 off, so the
                // range from 0 hz to 22050 hz lies between 0 and 1
                // (1378.12 hz inSOSPitch is 0.73).
                const auto add = [&](detail::ExpressionNode part) {
                    part.range = node.range;
                    return addNode(std::move(part));
                };
                const auto literal = [&](double value) {
                    detail::ExpressionNode constant;
                    constant.kind = detail::NodeKind::Literal;
                    constant.number = value;
                    return add(std::move(constant));
                };
                const auto call = [&](detail::FunctionOp function, NodeId first,
                                      std::optional<NodeId> second = std::nullopt) {
                    detail::ExpressionNode part;
                    part.kind = detail::NodeKind::Call;
                    part.functionOp = function;
                    part.arguments[0] = first;
                    part.argumentCount = 1;
                    if (second.has_value())
                    {
                        part.arguments[1] = *second;
                        part.argumentCount = 2;
                    }
                    return add(std::move(part));
                };
                const auto binary = [&](BinaryOp operation, NodeId left, NodeId right) {
                    detail::ExpressionNode part;
                    part.kind = detail::NodeKind::Binary;
                    part.binaryOp = operation;
                    part.left = left;
                    part.right = right;
                    return add(std::move(part));
                };
                detail::ExpressionNode inHertz;
                inHertz.kind = detail::NodeKind::UnitApplication;
                inHertz.unit = Unit::Hertz;
                inHertz.left = *receiver;
                const auto hertz = add(std::move(inHertz));
                const auto top = literal(22050.0);
                const auto lowest = literal(std::exp2(-15.0));
                const auto fifteen = literal(15.0);
                const auto one = literal(1.0);
                if (!hertz || !top || !lowest || !fifteen || !one)
                    return std::nullopt;
                const auto plain = call(detail::FunctionOp::RemoveUnits, *hertz);
                if (!plain)
                    return std::nullopt;
                const auto ratio = binary(BinaryOp::Divide, *plain, *top);
                if (!ratio)
                    return std::nullopt;
                const auto bounded = call(detail::FunctionOp::Max, *ratio, *lowest);
                if (!bounded)
                    return std::nullopt;
                const auto octaves = call(detail::FunctionOp::Log2, *bounded);
                if (!octaves)
                    return std::nullopt;
                const auto steps = binary(BinaryOp::Divide, *octaves, *fifteen);
                if (!steps)
                    return std::nullopt;
                node.kind = detail::NodeKind::Binary;
                node.binaryOp = BinaryOp::Add;
                node.left = *steps;
                node.right = *one;
            }
            else if (message.text == "countTriggers" || message.text == "asToggle"
                     || message.text == "durationBetweenTriggers"
                     || message.text == "durationOfGate")
            {
                // Messages to a trigger, with the
                // arguments their keyword forms take.
                const auto literal = [&](double value) {
                    detail::ExpressionNode constant;
                    constant.kind = detail::NodeKind::Literal;
                    constant.number = value;
                    constant.range = message.range;
                    return addNode(std::move(constant));
                };
                const auto first = literal(message.text == "countTriggers" ? 0.0 : 1.0);
                const auto second = literal(0.0);
                if (!first.has_value() || !second.has_value())
                    return std::nullopt;
                node.kind = detail::NodeKind::Call;
                node.arguments[0] = *receiver;
                node.arguments[1] = *first;
                node.argumentCount = 2;
                if (message.text == "countTriggers")
                {
                    node.functionOp = detail::FunctionOp::CountTriggers;
                    node.arguments[2] = *second;
                    node.argumentCount = 3;
                }
                else if (message.text == "asToggle")
                {
                    node.functionOp = detail::FunctionOp::Toggle;
                }
                else
                {
                    node.functionOp = detail::FunctionOp::TriggerDuration;
                    node.number = message.text == "durationOfGate" ? 1.0 : 0.0;
                }
            }
            else if (message.text == "updateInternalState" || message.text == "readEveryMillisecond")
            {
                // A time function updated for its state
                // alone, and an EventVariable that has its expression
                // evaluated every millisecond; neither leaves a value.
                node.kind = detail::NodeKind::Call;
                node.functionOp = message.text == "updateInternalState"
                    ? detail::FunctionOp::UpdateInternalState
                    : detail::FunctionOp::EveryMillisecond;
                node.arguments[0] = *receiver;
                node.argumentCount = 1;
            }
            else if (const auto function = valUnaryMessage(message.text))
            {
                node.kind = detail::NodeKind::Call;
                node.functionOp = *function;
                node.arguments[0] = *receiver;
                node.argumentCount = 1;
                // Format 156: in a project taking the table rounding
                // (RoundingRule::ManualTable), rounded adds 0.5 and truncates
                // toward negative infinity, and truncated rounds toward
                // negative infinity.
                if (definitions_ != nullptr && definitions_->rounding == RoundingRule::ManualTable)
                {
                    if (message.text == "rounded")
                        node.functionOp = detail::FunctionOp::RoundHalfUp;
                    else if (message.text == "truncated")
                        node.functionOp = detail::FunctionOp::Floor;
                }
                // A frequency written in bpm leaves its beats per minute
                // when its units go, as 4 s bpm removeUnits is 15, and so
                // does one a plain number multiplies or divides, as
                // (2.96961 s bpm * 4) removeUnits is 80.8187; a frequency is
                // held in hertz, so bpmWritten finds the unit it was written in.
                const auto written = *function == detail::FunctionOp::RemoveUnits
                    ? bpmWritten(*receiver) : std::nullopt;
                if (written.has_value())
                {
                    if (bpmFactorEnds_ != nullptr && storage_.nodes[*receiver].kind == detail::NodeKind::Binary)
                        bpmFactorEnds_->insert(bpmFactorEnds_->end(), written->begin(), written->end());
                    const auto plain = addNode(std::move(node));
                    detail::ExpressionNode minute;
                    minute.kind = detail::NodeKind::Literal;
                    minute.number = 60.0;
                    minute.range = message.range;
                    const auto seconds = addNode(std::move(minute));
                    if (!plain.has_value() || !seconds.has_value())
                        return std::nullopt;
                    node = {};
                    node.kind = detail::NodeKind::Binary;
                    node.binaryOp = BinaryOp::Multiply;
                    node.left = *plain;
                    node.right = *seconds;
                    node.range = storage_.nodes[*plain].range;
                }
            }
            else
            {
                addDiagnostic(
                    Diagnostic::Code::UnknownFunction,
                    message.range,
                    "Unknown Val message " + std::string{message.text});
                return std::nullopt;
            }
            advance();
            receiver = addNode(std::move(node));
        }
        return receiver;
    }

    // A minus written before a number makes a negative literal, as in
    // Smalltalk, so -3 abs is 3.
    std::optional<NodeId> parseValPrimary(std::size_t depth)
    {
        if (!admitDepth(depth))
            return std::nullopt;
        // A value in braces is Smalltalk's, worked
        // out before the Sound plays, as in !KeyNumber - 36 / {63 - 36};
        // here it groups as parentheses do.
        if (current_.kind == TokenKind::LeftBrace)
        {
            advance();
            const auto braced = parseValSequence(depth + 1);
            if (!braced.has_value())
                return std::nullopt;
            if (current_.kind != TokenKind::RightBrace)
            {
                addDiagnostic(Diagnostic::Code::UnexpectedToken, current_.range, "Expected '}' to close the braced value");
                return std::nullopt;
            }
            advance();
            return braced;
        }
        if (current_.kind == TokenKind::Minus)
        {
            const auto minus = current_;
            advance();
            if (current_.kind == TokenKind::Number)
            {
                const auto literal = parseValNumber();
                if (literal.has_value())
                {
                    auto& node = storage_.nodes[*literal];
                    node.number = -node.number;
                    node.range = joinRanges(minus.range, node.range);
                }
                return literal;
            }
            const auto operand = parseValPrimary(depth + 1);
            if (!operand.has_value())
                return std::nullopt;
            detail::ExpressionNode node;
            node.kind = detail::NodeKind::Unary;
            node.unaryOp = UnaryOp::Negate;
            node.left = *operand;
            node.range = joinRanges(minus.range, storage_.nodes[*operand].range);
            return addNode(std::move(node));
        }
        if (current_.kind == TokenKind::Number)
            return parseValNumber();
        if (current_.kind == TokenKind::ArrayStart)
        {
            addDiagnostic(
                Diagnostic::Code::UnexpectedToken,
                current_.range,
                "A literal array can only be the argument of of: or into:");
            return std::nullopt;
        }
        // on in a Duration field keeps a Sound on "forever (actually, just
        // 2 years)".
        if (current_.kind == TokenKind::Identifier && current_.text == "on")
        {
            detail::ExpressionNode node;
            node.kind = detail::NodeKind::Literal;
            node.number = 2.0 * 365.0 * 24.0 * 60.0 * 60.0;
            node.unit = Unit::Seconds;
            node.range = current_.range;
            advance();
            return addNode(std::move(node));
        }
        // Smalltalk's Float pi, Float phi and Double pi.
        if (current_.kind == TokenKind::Identifier
            && (current_.text == "Float" || current_.text == "Double"))
        {
            const auto receiver = current_;
            advance();
            const auto constant = current_.kind != TokenKind::Identifier
                ? std::optional<double>{}
                : current_.text == "pi"
                    ? std::optional<double>{std::numbers::pi}
                    : current_.text == "phi" && receiver.text == "Float"
                        ? std::optional<double>{std::numbers::phi}
                        : std::optional<double>{};
            if (!constant.has_value())
            {
                addDiagnostic(
                    Diagnostic::Code::UnknownFunction,
                    receiver.range,
                    receiver.text == "Float" ? "Float answers only pi and phi"
                                             : "Double answers only pi");
                return std::nullopt;
            }
            detail::ExpressionNode node;
            node.kind = detail::NodeKind::Literal;
            node.number = *constant;
            node.range = joinRanges(receiver.range, current_.range);
            advance();
            return addNode(std::move(node));
        }
        // !LocalTime, the seconds since the Sound started as a plain
        // number: a function of time, not a fader; and
        // !RealTime and !Time, the seconds since the whole Sound started,
        // in a source withWholeSoundClocks marked.
        const auto wholeSoundClock = wholeSoundClocks_ && current_.kind == TokenKind::Event
            && (current_.text == "RealTime" || current_.text == "Time");
        if (current_.kind == TokenKind::Event && (current_.text == "LocalTime" || wholeSoundClock))
        {
            const auto range = current_.range;
            const auto controlled = wholeSoundClock && current_.text == "Time";
            advance();
            detail::ExpressionNode trigger;
            trigger.kind = detail::NodeKind::Literal;
            trigger.number = 1.0;
            trigger.range = range;
            const auto triggerId = addNode(std::move(trigger));
            detail::ExpressionNode duration;
            duration.kind = detail::NodeKind::Literal;
            duration.number = 1.0;
            duration.unit = Unit::Seconds;
            duration.range = range;
            const auto durationId = addNode(std::move(duration));
            if (!triggerId.has_value() || !durationId.has_value())
                return std::nullopt;
            detail::ExpressionNode elapsed;
            elapsed.kind = detail::NodeKind::Call;
            elapsed.functionOp = detail::FunctionOp::TimeFunction;
            elapsed.number = controlled ? controlledWholeSoundTimeVariant
                : wholeSoundClock       ? wholeSoundTimeVariant
                                        : localTimeVariant;
            elapsed.arguments[0] = *triggerId;
            elapsed.arguments[1] = *durationId;
            elapsed.argumentCount = 2;
            elapsed.range = range;
            return addNode(std::move(elapsed));
        }
        // !TimingClock, a trigger 24 times a beat, and
        // !TimingClockDuration, the seconds of one of its triggers as a plain
        // number, at the tempo the Sound plays to.
        if (current_.kind == TokenKind::Event
            && (current_.text == "TimingClock" || current_.text == "TimingClockDuration"))
        {
            const auto range = current_.range;
            const auto duration = current_.text == "TimingClockDuration";
            advance();
            const auto literal = [&](double value, Unit unit) {
                detail::ExpressionNode node;
                node.kind = detail::NodeKind::Literal;
                node.number = value;
                node.unit = unit;
                node.range = range;
                return addNode(std::move(node));
            };
            const auto call = [&](detail::FunctionOp function, std::initializer_list<NodeId> values) {
                detail::ExpressionNode node;
                node.kind = detail::NodeKind::Call;
                node.functionOp = function;
                for (const auto value : values)
                    node.arguments[node.argumentCount++] = value;
                node.range = range;
                return addNode(std::move(node));
            };
            // A twenty-fourth of a beat in seconds, without its unit.
            const auto tick = literal(1.0 / 24.0, Unit::Beats);
            const auto seconds = tick.has_value() ? call(detail::FunctionOp::RemoveUnits, {*tick}) : std::nullopt;
            if (duration || !seconds.has_value())
                return seconds;
            // 1 bpm: (60 / those seconds), half a tick on.
            const auto one = literal(1.0, Unit::None);
            const auto minute = literal(60.0, Unit::None);
            const auto half = literal(0.5, Unit::None);
            if (!one.has_value() || !minute.has_value() || !half.has_value())
                return std::nullopt;
            detail::ExpressionNode rate;
            rate.kind = detail::NodeKind::Binary;
            rate.binaryOp = BinaryOp::Divide;
            rate.left = *minute;
            rate.right = *seconds;
            rate.range = range;
            const auto rateId = addNode(std::move(rate));
            if (!rateId.has_value())
                return std::nullopt;
            const auto standIn = call(detail::FunctionOp::Metronome, {*one, *rateId, *half});
            if (!standIn.has_value())
                return std::nullopt;
            // `MIDITimingClock extractClock, the
            // MIDI timing clock's trigger, once its messages come; the
            // stand-in until then.
            return midiTimingClock(*standIn, 1.0, range);
        }
        // The Event Source `MIDITimingClock, also
        // written `MIDITimingClock extractClock, a trigger once for each MIDI
        // timing clock message, 0 without them.
        if (current_.kind == TokenKind::MidiTimingClockSource)
        {
            auto range = current_.range;
            advance();
            if (current_.kind == TokenKind::Identifier && current_.text == "extractClock")
            {
                range = joinRanges(range, current_.range);
                advance();
            }
            detail::ExpressionNode zero;
            zero.kind = detail::NodeKind::Literal;
            zero.number = 0.0;
            zero.range = range;
            const auto zeroId = addNode(std::move(zero));
            if (!zeroId.has_value())
                return std::nullopt;
            return midiTimingClock(*zeroId, 0.0, range);
        }
        // SignalProcessor sampleRate and halfSampleRate, plain
        // numbers: the frequency of one
        // sample's period, without its unit.
        if (current_.kind == TokenKind::Identifier
            && current_.text == "SignalProcessor")
        {
            const auto receiver = current_;
            advance();
            // A wavetable's length and the default
            // bank sizes, ours being the prototypes' defaults (a CloudBank's
            // default BankSize).
            constexpr std::array<std::pair<std::string_view, double>, 6> constants{{
                {"tableLength", 4096.0},
                {"bankSizeForBandpassFilters", 8.0},
                {"bankSizeForFormantOscillator", 4.0},
                {"bankSizeForGrainCloud", 8.0},
                {"bankSizeForSOS", 8.0},
                {"bankSizeForVocoder", 16.0},
            }};
            if (current_.kind == TokenKind::Identifier)
            {
                const auto constant = std::ranges::find(constants, current_.text, &std::pair<std::string_view, double>::first);
                if (constant != constants.end())
                {
                    detail::ExpressionNode value;
                    value.kind = detail::NodeKind::Literal;
                    value.number = constant->second;
                    value.range = joinRanges(receiver.range, current_.range);
                    advance();
                    return addNode(std::move(value));
                }
            }
            if (current_.kind != TokenKind::Identifier
                || (current_.text != "sampleRate"
                    && current_.text != "halfSampleRate"
                    && current_.text != "inverseSampleRate"
                    && current_.text != "inverseHalfSampleRate"))
            {
                addDiagnostic(
                    Diagnostic::Code::UnknownFunction,
                    receiver.range,
                    "SignalProcessor answers sampleRate, halfSampleRate, their inverses, tableLength and its bank sizes");
                return std::nullopt;
            }
            const auto half = current_.text == "halfSampleRate";
            const auto inverse = current_.text.starts_with("inverse");
            const auto inverseHalf = current_.text == "inverseHalfSampleRate";
            const auto range = joinRanges(receiver.range, current_.range);
            advance();
            if (inverse)
            {
                // The seconds of one sample's period, or of two.
                detail::ExpressionNode samples;
                samples.kind = detail::NodeKind::Literal;
                samples.number = inverseHalf ? 2.0 : 1.0;
                samples.unit = Unit::Samples;
                samples.range = range;
                const auto samplesId = addNode(std::move(samples));
                if (!samplesId.has_value())
                    return std::nullopt;
                detail::ExpressionNode seconds;
                seconds.kind = detail::NodeKind::UnitApplication;
                seconds.unit = Unit::Seconds;
                seconds.left = *samplesId;
                seconds.range = range;
                const auto secondsId = addNode(std::move(seconds));
                if (!secondsId.has_value())
                    return std::nullopt;
                detail::ExpressionNode plain;
                plain.kind = detail::NodeKind::Call;
                plain.functionOp = detail::FunctionOp::RemoveUnits;
                plain.arguments[0] = *secondsId;
                plain.argumentCount = 1;
                plain.range = range;
                return addNode(std::move(plain));
            }
            detail::ExpressionNode sample;
            sample.kind = detail::NodeKind::Literal;
            sample.number = 1.0;
            sample.unit = Unit::Samples;
            sample.range = range;
            const auto sampleId = addNode(std::move(sample));
            if (!sampleId.has_value())
                return std::nullopt;
            detail::ExpressionNode frequency;
            frequency.kind = detail::NodeKind::UnitApplication;
            frequency.unit = Unit::Hertz;
            frequency.left = *sampleId;
            frequency.range = range;
            const auto frequencyId = addNode(std::move(frequency));
            if (!frequencyId.has_value())
                return std::nullopt;
            detail::ExpressionNode rate;
            rate.kind = detail::NodeKind::Call;
            rate.functionOp = detail::FunctionOp::RemoveUnits;
            rate.arguments[0] = *frequencyId;
            rate.argumentCount = 1;
            rate.range = range;
            const auto rateId = addNode(std::move(rate));
            if (!rateId.has_value() || !half)
                return rateId;
            detail::ExpressionNode halfValue;
            halfValue.kind = detail::NodeKind::Literal;
            halfValue.number = 0.5;
            halfValue.range = range;
            const auto halfId = addNode(std::move(halfValue));
            if (!halfId.has_value())
                return std::nullopt;
            detail::ExpressionNode product;
            product.kind = detail::NodeKind::Binary;
            product.binaryOp = BinaryOp::Multiply;
            product.left = *rateId;
            product.right = *halfId;
            product.range = range;
            return addNode(std::move(product));
        }
        return parsePrefix(depth);
    }

    // A number, a pitch such as 4 c sharp, or a number with a unit.
    // Any other name that follows is a message for the unary loop.
    std::optional<NodeId> parseValNumber()
    {
        if (current_.text.find(':') != std::string_view::npos)
            return parseTimeCodeLiteral();
        const auto numberToken = current_;
        // A number needs a numeral before its decimal point, 0.5 and not
        // .5.
        if (numberToken.text.front() == '.')
        {
            addDiagnostic(
                Diagnostic::Code::InvalidNumber,
                numberToken.range,
                "Nothing more expected->: a number needs its leading zero, so write 0"
                    + std::string{numberToken.text}
                    + ", as a period not after a numeral ends a statement");
            advance();
            return std::nullopt;
        }
        bool ok = false;
        const auto number = parseDouble(numberToken.text, ok);
        if (!ok)
        {
            addDiagnostic(
                Diagnostic::Code::InvalidNumber,
                numberToken.range,
                "Invalid number literal");
            advance();
            return std::nullopt;
        }
        detail::ExpressionNode node;
        node.kind = detail::NodeKind::Literal;
        node.number = number;
        node.range = numberToken.range;
        advance();
        if (current_.kind == TokenKind::Identifier)
        {
            if (const auto pitch = parsePitchName(current_.text))
            {
                node.number = (number + 1.0) * 12.0 + *pitch;
                node.unit = Unit::NoteNumber;
                node.range = joinRanges(node.range, current_.range);
                advance();
                if (current_.kind == TokenKind::Identifier
                    && (current_.text == "sharp" || current_.text == "flat"))
                {
                    node.number += current_.text == "sharp" ? 1.0 : -1.0;
                    node.range = joinRanges(node.range, current_.range);
                    advance();
                }
            }
            else if (const auto unit = valUnit(current_.text))
            {
                node.unit = *unit;
                node.range = joinRanges(node.range, current_.range);
                advance();
                // A beat at the project's tempo
                // for fields, when it keeps one.
                if (const auto seconds = fieldBeatSeconds(); seconds.has_value() && node.unit == Unit::Beats)
                {
                    node.number *= *seconds;
                    node.unit = Unit::Seconds;
                }
            }
        }
        return addNode(std::move(node));
    }

    std::optional<LiteralArray> parseLiteralArray(std::size_t depth)
    {
        LiteralArray array;
        array.range = current_.range;
        array.eager = current_.text.size() == 3;
        advance();
        while (!budgetExceeded_ && current_.kind != TokenKind::RightParen)
        {
            if (current_.kind == TokenKind::End)
            {
                addDiagnostic(
                    Diagnostic::Code::ExpectedRightParen,
                    current_.range,
                    "Expected ')' to close the literal array");
                return std::nullopt;
            }
            if (array.values.size() + array.xs.size()
                >= ExpressionLimits::maximumListElements)
            {
                failBudget(
                    current_.range,
                    "Expression exceeds the fixed list-element budget");
                return std::nullopt;
            }
            if (current_.kind != TokenKind::LeftBrace)
            {
                // A bare element is a number, a negative number or an
                // Event Value.
                const auto element = parseValPrimary(depth + 1);
                if (!element.has_value())
                    return std::nullopt;
                array.values.push_back(*element);
                continue;
            }
            advance();
            const auto first = parseKeywordMessage(depth + 1);
            if (!first.has_value())
                return std::nullopt;
            if (current_.kind == TokenKind::At)
            {
                advance();
                const auto second = parseKeywordMessage(depth + 1);
                if (!second.has_value())
                    return std::nullopt;
                array.xs.push_back(*first);
                array.ys.push_back(*second);
            }
            else
            {
                array.values.push_back(*first);
            }
            if (current_.kind != TokenKind::RightBrace)
            {
                addDiagnostic(
                    Diagnostic::Code::UnexpectedToken,
                    current_.range,
                    "Expected '}' to close the array element");
                return std::nullopt;
            }
            advance();
        }
        if (budgetExceeded_)
            return std::nullopt;
        array.range = joinRanges(array.range, current_.range);
        advance();
        if (!array.values.empty() && !array.xs.empty())
        {
            addDiagnostic(
                Diagnostic::Code::InvalidArgument,
                array.range,
                "A literal array holds either values or {x @ y} points");
            return std::nullopt;
        }
        return array;
    }

    // The MIDI timing clock's trigger, the stand-in
    // `standIn` until its first message when `withStandIn` is 1.
    std::optional<NodeId> midiTimingClock(NodeId standIn, double withStandIn, SourceRange range)
    {
        detail::ExpressionNode node;
        node.kind = detail::NodeKind::Call;
        node.functionOp = detail::FunctionOp::MidiTimingClock;
        node.number = withStandIn;
        node.arguments[0] = standIn;
        node.argumentCount = 1;
        node.range = range;
        return addNode(std::move(node));
    }

    // A Val bpm: is a sequence of !KeyDown-like triggers, each dipping
    // to -1 to steal its voice; !TimingClock's
    // pulses and the frozen profiles' metronomes do not.
    template <typename Call>
    std::optional<NodeId> metronome(const Call& call, std::initializer_list<NodeId> values, SourceRange range)
    {
        const auto stealing = languageVersion_ == ExpressionLanguageVersion::ValV1;
        if (stealing && metronomeRanges_ != nullptr)
            metronomeRanges_->push_back(range);
        return call(detail::FunctionOp::Metronome, values, stealing ? 1.0 : 0.0);
    }

    std::optional<NodeId> keywordMessage(
        const std::string& selector,
        NodeId receiver,
        std::span<const NodeId> arguments,
        const std::optional<LiteralArray>& array,
        SourceRange range)
    {
        // A pasted Sound's channel with its update period in milliseconds
        //.
        if (selector.size() == 2 && selector.back() == ':'
            && isPastedSoundChannel(std::string_view{selector}.substr(0, 1))
            && !array.has_value() && arguments.size() == 1
            && isPlainIdentifier(receiver))
        {
            const auto& period = storage_.nodes[arguments[0]];
            if (period.kind != detail::NodeKind::Literal
                || (period.unit != Unit::None && period.unit != Unit::Milliseconds)
                || !std::isfinite(period.number) || period.number <= 0.0)
            {
                addDiagnostic(
                    Diagnostic::Code::UnexpectedToken,
                    range,
                    "A pasted Sound's update period must be a positive number of milliseconds");
                return std::nullopt;
            }
            if (!nameSoundReference(receiver, std::string_view{selector}.substr(0, 1),
                    period.number, range))
            {
                return std::nullopt;
            }
            return receiver;
        }
        const auto binary = [&](BinaryOp operation) {
            detail::ExpressionNode node;
            node.kind = detail::NodeKind::Binary;
            node.binaryOp = operation;
            node.left = receiver;
            node.right = arguments[0];
            node.range = range;
            return addNode(std::move(node));
        };
        const auto comparison = [&](detail::ComparisonOp operation) {
            detail::ExpressionNode node;
            node.kind = detail::NodeKind::Comparison;
            node.comparisonOp = operation;
            node.left = receiver;
            node.right = arguments[0];
            node.range = range;
            return addNode(std::move(node));
        };
        const auto call = [&](detail::FunctionOp function,
                              std::initializer_list<NodeId> values,
                              double number = 0.0) {
            detail::ExpressionNode node;
            node.kind = detail::NodeKind::Call;
            node.functionOp = function;
            node.number = number;
            for (const auto value : values)
                node.arguments[node.argumentCount++] = value;
            node.range = range;
            return addNode(std::move(node));
        };
        const auto literal = [&](double value) {
            detail::ExpressionNode node;
            node.kind = detail::NodeKind::Literal;
            node.number = value;
            node.range = range;
            return addNode(std::move(node));
        };

        if (array.has_value())
        {
            if (selector == "of:" && !array->values.empty())
            {
                const auto index = call(
                    detail::FunctionOp::ArrayIndex,
                    {receiver},
                    static_cast<double>(array->values.size()));
                if (!index.has_value())
                    return std::nullopt;
                return selection(*index, array->values, range, array->eager);
            }
            if (selector == "into:" && array->xs.size() >= 2)
            {
                const auto linear = literal(0.5);
                if (!linear.has_value()
                    || storage_.listElements.size()
                        > ExpressionLimits::maximumNodes
                            - 2 * array->xs.size())
                {
                    return std::nullopt;
                }
                const auto offset = storage_.listElements.size();
                storage_.listElements.insert(
                    storage_.listElements.end(),
                    array->xs.begin(),
                    array->xs.end());
                storage_.listElements.insert(
                    storage_.listElements.end(),
                    array->ys.begin(),
                    array->ys.end());
                detail::ExpressionNode node;
                node.kind = detail::NodeKind::ArrayMap;
                node.left = receiver;
                node.right = *linear;
                node.listElementOffset = static_cast<std::uint16_t>(offset);
                node.listElementCount =
                    static_cast<std::uint8_t>(array->xs.size());
                node.range = range;
                return addNode(std::move(node));
            }
        }
        else if (arguments.size() == 1)
        {
            if (selector == "mod:")
                return binary(BinaryOp::Modulo);
            if (selector == "rem:")
                return binary(BinaryOp::Remainder);
            // quo: divides and truncates toward zero.
            if (selector == "quo:")
            {
                const auto quotient = binary(BinaryOp::Divide);
                return quotient.has_value()
                    ? call(detail::FunctionOp::Truncate, {*quotient})
                    : quotient;
            }
            if (selector == "raisedTo:")
                return binary(BinaryOp::Power);
            if (selector == "vmin:")
                return call(detail::FunctionOp::Min, {receiver, arguments[0]});
            if (selector == "vmax:")
                return call(detail::FunctionOp::Max, {receiver, arguments[0]});
            if (selector == "roundTo:")
                return call(detail::FunctionOp::RoundTo, {receiver, arguments[0]});
            // Smalltalk's messages to numbers.
            if (selector == "max:")
                return call(detail::FunctionOp::Max, {receiver, arguments[0]});
            if (selector == "min:")
                return call(detail::FunctionOp::Min, {receiver, arguments[0]});
            if (selector == "maxAbs:")
                return call(detail::FunctionOp::MaxAbs, {receiver, arguments[0]});
            if (selector == "minAbs:")
                return call(detail::FunctionOp::MinAbs, {receiver, arguments[0]});
            if (selector == "truncateTo:")
                return call(detail::FunctionOp::TruncateTo, {receiver, arguments[0]});
            if (selector == "ceilingTo:")
                return call(detail::FunctionOp::CeilingTo, {receiver, arguments[0]});
            if (selector == "log:")
                return call(detail::FunctionOp::LogBase, {receiver, arguments[0]});
            if (selector == "floorLog:")
                return call(detail::FunctionOp::FloorLog, {receiver, arguments[0]});
            if (selector == "ramp:")
                return call(detail::FunctionOp::Ramp, {receiver, arguments[0]});
            constexpr std::array<std::pair<std::string_view, double>, 5> ramps{{
                {"fullRamp:", 1.0},
                {"triangle:", 2.0},
                {"repeatingRamp:", 4.0},
                {"repeatingFullRamp:", 5.0},
                {"repeatingTriangle:", 6.0},
            }};
            for (const auto& [name, variant] : ramps)
            {
                if (selector == name)
                {
                    return call(detail::FunctionOp::TimeFunction,
                        {receiver, arguments[0]}, variant);
                }
            }
            if (selector == "smooth:")
                return call(detail::FunctionOp::Smooth, {receiver, arguments[0]});
            // A gate from the receiver's time code for the argument's
            // duration.
            if (selector == "gateOnAtTimeCodeForDuration:")
            {
                const auto from = midiTimeCodeReference(range);
                const auto until = midiTimeCodeReference(range);
                if (!from.has_value() || !until.has_value())
                    return std::nullopt;
                const auto node = [&](detail::ExpressionNode value) {
                    value.range = range;
                    return addNode(std::move(value));
                };
                detail::ExpressionNode started;
                started.kind = detail::NodeKind::Comparison;
                started.comparisonOp = detail::ComparisonOp::GreaterEqual;
                started.left = *from;
                started.right = receiver;
                detail::ExpressionNode end;
                end.kind = detail::NodeKind::Binary;
                end.binaryOp = BinaryOp::Add;
                end.left = receiver;
                end.right = arguments[0];
                const auto endNode = node(std::move(end));
                if (!endNode.has_value())
                    return std::nullopt;
                detail::ExpressionNode before;
                before.kind = detail::NodeKind::Comparison;
                before.comparisonOp = detail::ComparisonOp::Less;
                before.left = *until;
                before.right = *endNode;
                const auto startedNode = node(std::move(started));
                const auto beforeNode = node(std::move(before));
                if (!startedNode.has_value() || !beforeNode.has_value())
                    return std::nullopt;
                detail::ExpressionNode gate;
                gate.kind = detail::NodeKind::Binary;
                gate.binaryOp = BinaryOp::Multiply;
                gate.left = *startedNode;
                gate.right = *beforeNode;
                return node(std::move(gate));
            }
            if (selector == "bpm:")
            {
                const auto duty = literal(0.5);
                if (!duty.has_value())
                    return std::nullopt;
                return metronome(call, {receiver, arguments[0], *duty}, range);
            }
            // Messages to a trigger.
            if (selector == "countTriggersMod:")
            {
                const auto forward = literal(0.0);
                if (!forward.has_value())
                    return std::nullopt;
                return call(detail::FunctionOp::CountTriggers, {receiver, arguments[0], *forward}, 1.0);
            }
            if (selector == "triggerEvery:")
                return call(detail::FunctionOp::TriggerEvery, {receiver, arguments[0]});
            if (selector == "toggleWhen:")
                return call(detail::FunctionOp::Toggle, {receiver, arguments[0]});
            if (selector == "gateWhen:")
                return call(detail::FunctionOp::GateWhen, {receiver, arguments[0]});
            if (selector == "durationBetweenTriggers:")
                return call(detail::FunctionOp::TriggerDuration, {receiver, arguments[0]});
            if (selector == "hasChangedReset:")
                return call(detail::FunctionOp::ChangedReset, {receiver, arguments[0]});
            if (selector == "sustainBy:")
                return call(detail::FunctionOp::SustainBy, {receiver, arguments[0]});
            constexpr std::array<std::pair<std::string_view, double>, 3> seeded{{
                {"nextRandomWithSeed:", 0.0},
                {"nextNormalWithSeed:", 1.0},
                {"nextRandExpWithSeed:", 2.0},
            }};
            for (const auto& [name, distribution] : seeded)
            {
                if (selector == name)
                {
                    return call(detail::FunctionOp::NextRandomSeeded,
                        {receiver, arguments[0]}, distribution);
                }
            }
            if (selector == "nextChaotic:")
            {
                const auto seed = literal(0.5);
                const auto reset = literal(0.0);
                if (!seed.has_value() || !reset.has_value())
                    return std::nullopt;
                return call(detail::FunctionOp::Chaotic, {receiver, arguments[0], *seed, *reset});
            }
            if (selector == "sampleAndHold:")
            {
                const auto initial = literal(0.0);
                if (!initial.has_value())
                    return std::nullopt;
                return call(detail::FunctionOp::SampleHold, {arguments[0], receiver, *initial});
            }
            if (selector == "trackAndHold:")
                return call(detail::FunctionOp::TrackHold, {receiver, arguments[0]});
            if (selector == "eq:")
                return comparison(detail::ComparisonOp::Equal);
            if (selector == "ne:")
                return comparison(detail::ComparisonOp::NotEqual);
            if (selector == "gt:" || selector == "threshold:")
                return comparison(detail::ComparisonOp::Greater);
            if (selector == "lt:")
                return comparison(detail::ComparisonOp::Less);
            if (selector == "ge:")
                return comparison(detail::ComparisonOp::GreaterEqual);
            if (selector == "le:")
                return comparison(detail::ComparisonOp::LessEqual);
        }
        else if (selector == "between:and:" && arguments.size() == 2)
        {
            return call(detail::FunctionOp::Between,
                {receiver, arguments[0], arguments[1]});
        }
        else if (selector == "bpm:dutyCycle:" && arguments.size() == 2)
        {
            return metronome(call, {receiver, arguments[0], arguments[1]}, range);
        }
        else if (selector == "countTriggersMod:reverse:" && arguments.size() == 2)
        {
            return call(detail::FunctionOp::CountTriggers,
                {receiver, arguments[0], arguments[1]}, 2.0);
        }
        else if (selector == "nextChaotic:seed:" && arguments.size() == 2)
        {
            const auto reset = literal(0.0);
            if (!reset.has_value())
                return std::nullopt;
            return call(detail::FunctionOp::Chaotic,
                {receiver, arguments[0], arguments[1], *reset}, 1.0);
        }
        else if (selector == "nextChaotic:reset:" && arguments.size() == 2)
        {
            const auto seed = literal(0.5);
            if (!seed.has_value())
                return std::nullopt;
            return call(detail::FunctionOp::Chaotic,
                {receiver, arguments[0], *seed, arguments[1]}, 2.0);
        }
        else if (selector == "swarmFollowFromPosition:velocity:acceleration:friction:" && arguments.size() == 4)
        {
            return call(detail::FunctionOp::SwarmFollow,
                {receiver, arguments[0], arguments[1], arguments[2], arguments[3]});
        }
        else if (selector == "nextChaotic:seed:reset:" && arguments.size() == 3)
        {
            return call(detail::FunctionOp::Chaotic,
                {receiver, arguments[0], arguments[1], arguments[2]}, 3.0);
        }
        else if (selector == "false:true:" && arguments.size() == 2)
        {
            // A positive receiver is true; only the chosen branch runs
            //.
            const auto index = call(detail::FunctionOp::LogicValue, {receiver});
            if (!index.has_value())
                return std::nullopt;
            const std::array branches{arguments[0], arguments[1]};
            return selection(*index, branches, range);
        }
        else if (selector == "neg:zero:pos:initially:" && arguments.size() == 4)
        {
            // The initial value holds until the receiver has a value of its
            // own, until an Event Value it reads first changes; all
            // its expressions are evaluated. A receiver that reads no Event
            // Value has its value at once (1 neg: 4 zero: 3 pos: 2
            // initially: 8 is 2).
            if (!readsEventValue(receiver))
            {
                const auto sign = call(detail::FunctionOp::Sign, {receiver});
                const auto one = literal(1.0);
                if (!sign.has_value() || !one.has_value())
                    return std::nullopt;
                detail::ExpressionNode index;
                index.kind = detail::NodeKind::Binary;
                index.binaryOp = BinaryOp::Add;
                index.left = *sign;
                index.right = *one;
                index.range = range;
                const auto position = addNode(std::move(index));
                if (!position.has_value())
                    return std::nullopt;
                return selection(*position, std::span{arguments}.first(3), range);
            }
            return call(detail::FunctionOp::Initially,
                {receiver, arguments[0], arguments[1], arguments[2], arguments[3]});
        }
        else if (selector == "true:false:" && arguments.size() == 2)
        {
            // A positive receiver is true; only the chosen branch runs.
            const auto index = call(detail::FunctionOp::LogicValue, {receiver});
            if (!index.has_value())
                return std::nullopt;
            const std::array branches{arguments[1], arguments[0]};
            return selection(*index, branches, range);
        }
        else if (selector == "neg:zero:pos:" && arguments.size() == 3)
        {
            if (signSelectionRanges_ != nullptr)
                signSelectionRanges_->push_back(range);
            const auto sign = call(detail::FunctionOp::Sign, {receiver});
            const auto one = literal(1.0);
            if (!sign.has_value() || !one.has_value())
                return std::nullopt;
            detail::ExpressionNode index;
            index.kind = detail::NodeKind::Binary;
            index.binaryOp = BinaryOp::Add;
            index.left = *sign;
            index.right = *one;
            index.range = range;
            const auto position = addNode(std::move(index));
            if (!position.has_value())
                return std::nullopt;
            return selection(*position, arguments, range);
        }
        addDiagnostic(
            Diagnostic::Code::UnknownFunction,
            range,
            "Unknown Val message " + selector);
        return std::nullopt;
    }

    // Whether a node reads an Event Value, directly or through its operands.
    [[nodiscard]] bool readsEventValue(NodeId id) const
    {
        const auto& node = storage_.nodes[id];
        switch (node.kind)
        {
        case detail::NodeKind::EventReference:
            return true;
        case detail::NodeKind::Literal:
        case detail::NodeKind::VariableReference:
        case detail::NodeKind::IdentifierReference:
        case detail::NodeKind::FormalReference:
            return false;
        case detail::NodeKind::Unary:
        case detail::NodeKind::UnitApplication:
            return readsEventValue(node.left);
        case detail::NodeKind::Binary:
        case detail::NodeKind::Comparison:
            return readsEventValue(node.left) || readsEventValue(node.right);
        default:
            break;
        }
        for (std::size_t index = 0; index < node.argumentCount; ++index)
        {
            if (readsEventValue(node.arguments[index]))
                return true;
        }
        for (std::size_t index = 0; index < node.listElementCount; ++index)
        {
            if (readsEventValue(storage_.listElements[node.listElementOffset + index]))
                return true;
        }
        // Lists, series and user functions are taken to read one.
        return node.kind != detail::NodeKind::Call;
    }

    // Evaluates only the element the index selects, or each element on
    // every tick when `eager`, as a braced collect: program's Array is
    // (an arpeggiator's latches each take their own key down).
    std::optional<NodeId> selection(
        NodeId index,
        std::span<const NodeId> elements,
        SourceRange range,
        bool eager = false)
    {
        if (elements.empty()
            || elements.size() > ExpressionLimits::maximumListElements
            || storage_.listElements.size()
                > ExpressionLimits::maximumNodes - elements.size())
        {
            failBudget(range, "Expression exceeds the fixed list-element budget");
            return std::nullopt;
        }
        const auto offset = storage_.listElements.size();
        storage_.listElements.insert(
            storage_.listElements.end(), elements.begin(), elements.end());
        detail::ExpressionNode node;
        node.kind = detail::NodeKind::ListIndex;
        node.left = index;
        node.listElementOffset = static_cast<std::uint16_t>(offset);
        node.listElementCount = static_cast<std::uint8_t>(elements.size());
        node.number = eager ? 1.0 : 0.0;
        node.range = range;
        return addNode(std::move(node));
    }

    std::optional<NodeId> parsePrefix(std::size_t depth)
    {
        const auto token = current_;
        switch (current_.kind)
        {
        case TokenKind::Number:
            return parseNumberLiteral();
        case TokenKind::Minus:
        {
            advance();
            const auto negatesLiteral = current_.kind == TokenKind::Number;
            auto operand = parseExpression(unaryOperandBindingPower, depth + 1);
            if (!operand.has_value())
                return std::nullopt;
            auto& operandNode = storage_.nodes[*operand];
            if (negatesLiteral
                && operandNode.kind == detail::NodeKind::Literal
                && operandNode.unit == Unit::Decibels)
            {
                operandNode.number = -operandNode.number;
                operandNode.range = joinRanges(
                    token.range,
                    operandNode.range);
                return operand;
            }
            detail::ExpressionNode node;
            node.kind = detail::NodeKind::Unary;
            node.unaryOp = UnaryOp::Negate;
            node.left = *operand;
            node.range = joinRanges(token.range, storage_.nodes[*operand].range);
            return addNode(std::move(node));
        }
        case TokenKind::LeftParen:
        {
            advance();
            auto expression = usesValMessages(languageVersion_)
                ? parseValSequence(depth + 1)
                : parseExpression(0, depth + 1);
            if (!expression.has_value())
                return std::nullopt;
            if (current_.kind != TokenKind::RightParen)
            {
                addDiagnostic(
                    Diagnostic::Code::ExpectedRightParen,
                    current_.range,
                    "Expected ')' to close grouped expression");
                return expression;
            }
            advance();
            return expression;
        }
        case TokenKind::LeftBracket:
            if (isCompiledExpressionLanguage(languageVersion_))
                return parseListIndex(depth + 1);
            break;
        case TokenKind::Event:
        {
            detail::ExpressionNode node;
            node.kind = detail::NodeKind::EventReference;
            node.name = std::string(current_.text);
            node.range = current_.range;
            if (!addUniqueReference(
                    storage_.eventReferences,
                    current_.text,
                    ExpressionLimits::maximumEventReferences,
                    current_.range,
                    "event-reference"))
            {
                return std::nullopt;
            }
            advance();
            return addNode(std::move(node));
        }
        case TokenKind::Variable:
        {
            detail::ExpressionNode node;
            node.kind = detail::NodeKind::VariableReference;
            node.name = std::string(current_.text);
            node.range = current_.range;
            if (!addUniqueReference(
                    storage_.variableReferences,
                    current_.text,
                    ExpressionLimits::maximumVariableReferences,
                    current_.range,
                    "variable-reference"))
            {
                return std::nullopt;
            }
            advance();
            return addNode(std::move(node));
        }
        case TokenKind::Identifier:
        {
            const auto nameToken = current_;
            advance();
            if (current_.kind == TokenKind::LeftParen
                && isCompiledExpressionLanguage(languageVersion_))
            {
                if (nameToken.text == "series"
                    && supportsPureExpressionSeries(languageVersion_))
                {
                    return parseSeriesIndex(nameToken, depth + 1);
                }
                if (nameToken.text == "intoXArray"
                    && supportsArrayMap(languageVersion_))
                    return parseArrayMap(nameToken, depth + 1);
                const auto function = availableFunctionFor(
                    nameToken.text,
                    languageVersion_);
                if (function.has_value())
                    return parseFunctionCall(nameToken, *function, depth + 1);
                if (supportsExpressionDefinitions(languageVersion_)
                    && definitions_ != nullptr)
                {
                    const auto found = std::ranges::find_if(
                        definitions_->definitions,
                        [&nameToken](const ExpressionDefinition& definition) {
                            return definition.name == nameToken.text;
                        });
                    if (found != definitions_->definitions.end())
                    {
                        return parseUserCall(
                            nameToken,
                            static_cast<std::size_t>(
                                found - definitions_->definitions.begin()),
                            depth + 1);
                    }
                }
                if (!function.has_value())
                {
                    addDiagnostic(
                        Diagnostic::Code::UnknownFunction,
                        nameToken.range,
                        "Unknown pure expression function");
                    return std::nullopt;
                }
            }

            if (formals_ != nullptr)
            {
                const auto formal = std::ranges::find_if(
                    *formals_,
                    [&nameToken](
                        const ExpressionDefinitionParameter& parameter) {
                        return parameter.name == nameToken.text;
                    });
                if (formal != formals_->end())
                {
                    detail::ExpressionNode node;
                    node.kind = detail::NodeKind::FormalReference;
                    node.name = std::string(nameToken.text);
                    node.range = nameToken.range;
                    node.referenceIndex = static_cast<std::uint16_t>(
                        static_cast<std::size_t>(formal - formals_->begin()));
                    return addNode(std::move(node));
                }
            }

            // Nil, nothing.
            if (usesValMessages(languageVersion_) && nameToken.text == "nil")
            {
                detail::ExpressionNode nothing;
                nothing.kind = detail::NodeKind::Call;
                nothing.functionOp = detail::FunctionOp::Nothing;
                nothing.range = nameToken.range;
                return addNode(std::move(nothing));
            }
            detail::ExpressionNode node;
            node.kind = detail::NodeKind::IdentifierReference;
            node.name = std::string(nameToken.text);
            node.range = nameToken.range;
            // talasRecurrence's own value is no identifier to bind; outside
            // one, compiling names it and binding refuses it.
            if (nameToken.text != "talasRecurrentValue"
                && !addUniqueReference(
                    storage_.identifiers,
                    nameToken.text,
                    ExpressionLimits::maximumIdentifiers,
                    nameToken.range,
                    "identifier"))
            {
                return std::nullopt;
            }
            const auto id = addNode(std::move(node));
            if (current_.kind == TokenKind::LeftParen)
            {
                addDiagnostic(
                    Diagnostic::Code::UnexpectedToken,
                    current_.range,
                    "Function calls are not supported in parameter expressions");
            }
            return id;
        }
        case TokenKind::InvalidCharacter:
            addDiagnostic(
                Diagnostic::Code::UnexpectedCharacter,
                current_.range,
                "Unexpected character in expression");
            advance();
            return std::nullopt;
        case TokenKind::InvalidEvent:
            addDiagnostic(
                Diagnostic::Code::ExpectedExpression,
                current_.range,
                "Expected event name after '!'");
            advance();
            return std::nullopt;
        case TokenKind::InvalidVoiceNumber:
            addDiagnostic(Diagnostic::Code::UnknownIdentifier, current_.range,
                "Only ?VoiceNumber is defined in the voice expression profile");
            advance();
            return std::nullopt;
        case TokenKind::InvalidVariable:
            addDiagnostic(
                Diagnostic::Code::ExpectedExpression,
                current_.range,
                "Expected variable name after '@'");
            advance();
            return std::nullopt;
        case TokenKind::InvalidEventSource:
            addDiagnostic(
                Diagnostic::Code::ExpectedExpression,
                current_.range,
                current_.text.substr(1).starts_with("MIDIController")
                    ? "A MIDI controller's Event Source is `MIDIController00 to `MIDIController127"
                    : "Expected an Event Source's name, a letter first, after '`'");
            advance();
            return std::nullopt;
        case TokenKind::End:
        case TokenKind::Plus:
        case TokenKind::Star:
        case TokenKind::Slash:
        case TokenKind::Remainder:
        case TokenKind::Ramp:
        case TokenKind::RightParen:
        case TokenKind::RightBracket:
        case TokenKind::Comma:
        case TokenKind::Less:
        case TokenKind::LessEqual:
        case TokenKind::Greater:
        case TokenKind::GreaterEqual:
        case TokenKind::EqualEqual:
        case TokenKind::NotEqual:
        case TokenKind::Keyword:
        case TokenKind::Power:
        case TokenKind::IntegerDivide:
        case TokenKind::Modulo:
        case TokenKind::ArrayStart:
        case TokenKind::LeftBrace:
        case TokenKind::RightBrace:
        case TokenKind::At:
        case TokenKind::AssignKeep:
        case TokenKind::AssignPop:
        case TokenKind::MidiTimingClockSource:
            addDiagnostic(
                Diagnostic::Code::ExpectedExpression,
                current_.range,
                "Expected expression");
            return std::nullopt;
        }
        return std::nullopt;
    }

    std::optional<NodeId> parseListIndex(std::size_t depth)
    {
        if (!admitDepth(depth))
            return std::nullopt;
        const auto listStart = current_.range;
        advance();
        if (current_.kind == TokenKind::RightBracket)
        {
            addDiagnostic(
                Diagnostic::Code::InvalidArgumentCount,
                joinRanges(listStart, current_.range),
                "Pure expression selection list cannot be empty");
            return std::nullopt;
        }

        std::array<NodeId,
                   ExpressionLimits::maximumListElements> elements{};
        std::size_t elementCount = 0;
        while (!budgetExceeded_)
        {
            if (elementCount >= ExpressionLimits::maximumListElements)
            {
                failBudget(
                    current_.range,
                    "Expression exceeds the fixed list-element budget");
                return std::nullopt;
            }
            const auto element = parseExpression(0, depth + 1);
            if (!element.has_value())
                return std::nullopt;
            elements[elementCount++] = *element;
            if (current_.kind == TokenKind::RightBracket)
                break;
            if (current_.kind != TokenKind::Comma)
            {
                addDiagnostic(
                    Diagnostic::Code::ExpectedComma,
                    current_.range,
                    "Expected ',' between selection-list elements");
                return std::nullopt;
            }
            advance();
        }

        if (current_.kind != TokenKind::RightBracket)
        {
            addDiagnostic(
                Diagnostic::Code::UnexpectedToken,
                current_.range,
                "Expected ']' to close selection list");
            return std::nullopt;
        }
        const auto listEnd = current_.range;
        advance();
        if (current_.kind != TokenKind::LeftBracket)
        {
            addDiagnostic(
                Diagnostic::Code::UnexpectedToken,
                current_.range,
                "Selection list must be followed by an index");
            return std::nullopt;
        }
        advance();
        const auto index = parseExpression(0, depth + 1);
        if (!index.has_value())
            return std::nullopt;
        if (current_.kind != TokenKind::RightBracket)
        {
            addDiagnostic(
                Diagnostic::Code::UnexpectedToken,
                current_.range,
                "Expected ']' to close selection-list index");
            return std::nullopt;
        }
        const auto indexEnd = current_.range;
        advance();

        if (storage_.listElements.size()
                > ExpressionLimits::maximumNodes
                    - elementCount)
        {
            failBudget(
                listStart,
                "Expression exceeds the fixed list-element budget");
            return std::nullopt;
        }
        const auto elementOffset = storage_.listElements.size();
        storage_.listElements.insert(
            storage_.listElements.end(),
            elements.begin(),
            elements.begin()
                + static_cast<std::ptrdiff_t>(elementCount));

        detail::ExpressionNode node;
        node.kind = detail::NodeKind::ListIndex;
        node.left = *index;
        node.listElementOffset = static_cast<std::uint16_t>(elementOffset);
        node.listElementCount = static_cast<std::uint8_t>(elementCount);
        node.range = joinRanges(
            joinRanges(listStart, listEnd),
            indexEnd);
        return addNode(std::move(node));
    }

    std::optional<std::vector<NodeId>> parseArrayMapList(
        std::size_t depth)
    {
        if (!admitDepth(depth) || current_.kind != TokenKind::LeftBracket)
        {
            addDiagnostic(
                Diagnostic::Code::UnexpectedToken,
                current_.range,
                "intoXArray requires a bracketed array");
            return std::nullopt;
        }
        advance();
        std::vector<NodeId> elements;
        elements.reserve(15);
        while (!budgetExceeded_ && current_.kind != TokenKind::RightBracket)
        {
            if (elements.size() == 15)
            {
                failBudget(current_.range, "intoXArray supports at most 15 points");
                return std::nullopt;
            }
            const auto element = parseExpression(0, depth + 1);
            if (!element.has_value())
                return std::nullopt;
            elements.push_back(*element);
            if (current_.kind == TokenKind::RightBracket)
                break;
            if (current_.kind != TokenKind::Comma)
            {
                addDiagnostic(
                    Diagnostic::Code::ExpectedComma,
                    current_.range,
                    "Expected ',' between intoXArray members");
                return std::nullopt;
            }
            advance();
        }
        if (current_.kind != TokenKind::RightBracket)
        {
            addDiagnostic(
                Diagnostic::Code::UnexpectedToken,
                current_.range,
                "Expected ']' to close intoXArray array");
            return std::nullopt;
        }
        advance();
        if (elements.size() < 2)
        {
            addDiagnostic(
                Diagnostic::Code::InvalidArgumentCount,
                current_.range,
                "intoXArray requires at least two points");
            return std::nullopt;
        }
        return elements;
    }

    std::optional<NodeId> parseArrayMap(
        Token nameToken,
        std::size_t depth)
    {
        if (!admitDepth(depth))
            return std::nullopt;
        advance();
        const auto input = parseExpression(0, depth + 1);
        if (!input.has_value())
            return std::nullopt;
        const auto requireComma = [&]() {
            if (current_.kind != TokenKind::Comma)
            {
                addDiagnostic(
                    Diagnostic::Code::ExpectedComma,
                    current_.range,
                    "intoXArray requires input, X array, Y array, and interpolation");
                return false;
            }
            advance();
            return true;
        };
        if (!requireComma())
            return std::nullopt;
        const auto xs = parseArrayMapList(depth + 1);
        if (!xs.has_value() || !requireComma())
            return std::nullopt;
        const auto ys = parseArrayMapList(depth + 1);
        if (!ys.has_value() || !requireComma())
            return std::nullopt;
        const auto interpolation = parseExpression(0, depth + 1);
        if (!interpolation.has_value())
            return std::nullopt;
        if (current_.kind != TokenKind::RightParen)
        {
            addDiagnostic(
                Diagnostic::Code::ExpectedRightParen,
                current_.range,
                "Expected ')' to close intoXArray");
            return std::nullopt;
        }
        const auto endRange = current_.range;
        advance();
        if (xs->size() != ys->size()
            || storage_.listElements.size()
                > ExpressionLimits::maximumNodes - xs->size() - ys->size())
        {
            addDiagnostic(
                Diagnostic::Code::InvalidArgumentCount,
                joinRanges(nameToken.range, endRange),
                "intoXArray requires equal fixed-length arrays within the node budget");
            return std::nullopt;
        }
        const auto offset = storage_.listElements.size();
        storage_.listElements.insert(
            storage_.listElements.end(), xs->begin(), xs->end());
        storage_.listElements.insert(
            storage_.listElements.end(), ys->begin(), ys->end());
        detail::ExpressionNode node;
        node.kind = detail::NodeKind::ArrayMap;
        node.left = *input;
        node.right = *interpolation;
        node.listElementOffset = static_cast<std::uint16_t>(offset);
        node.listElementCount = static_cast<std::uint8_t>(xs->size());
        node.range = joinRanges(nameToken.range, endRange);
        return addNode(std::move(node));
    }

    std::optional<NodeId> parseFunctionCall(
        Token nameToken,
        detail::FunctionOp function,
        std::size_t depth)
    {
        if (!admitDepth(depth))
            return std::nullopt;
        advance();
        detail::ExpressionNode node;
        node.kind = detail::NodeKind::Call;
        node.functionOp = function;
        node.range = nameToken.range;

        if (current_.kind != TokenKind::RightParen)
        {
            while (!budgetExceeded_)
            {
                if (node.argumentCount >= node.arguments.size())
                {
                    addDiagnostic(
                        Diagnostic::Code::InvalidArgumentCount,
                        nameToken.range,
                        "Pure expression function has too many arguments");
                    return std::nullopt;
                }
                const auto argument = parseExpression(0, depth + 1);
                if (!argument.has_value())
                    return std::nullopt;
                node.arguments[node.argumentCount++] = *argument;
                if (current_.kind == TokenKind::RightParen)
                    break;
                if (current_.kind != TokenKind::Comma)
                {
                    addDiagnostic(
                        Diagnostic::Code::ExpectedComma,
                        current_.range,
                        "Expected ',' between function arguments");
                    return std::nullopt;
                }
                advance();
            }
        }

        if (current_.kind != TokenKind::RightParen)
        {
            addDiagnostic(
                Diagnostic::Code::ExpectedRightParen,
                current_.range,
                "Expected ')' to close function call");
            return std::nullopt;
        }
        node.range = joinRanges(node.range, current_.range);
        advance();
        if (node.argumentCount != functionArity(function))
        {
            addDiagnostic(
                Diagnostic::Code::InvalidArgumentCount,
                node.range,
                "Pure expression function has the wrong number of arguments");
            return std::nullopt;
        }
        return addNode(std::move(node));
    }

    std::optional<NodeId> parseSeriesIndex(
        Token nameToken,
        std::size_t depth)
    {
        if (!admitDepth(depth))
            return std::nullopt;

        advance();
        detail::ExpressionNode node;
        node.kind = detail::NodeKind::SeriesIndex;
        node.range = nameToken.range;

        while (!budgetExceeded_
               && current_.kind != TokenKind::RightParen)
        {
            if (node.argumentCount >= 3)
            {
                addDiagnostic(
                    Diagnostic::Code::InvalidArgumentCount,
                    nameToken.range,
                    "Pure expression series has too many arguments");
                return std::nullopt;
            }
            const auto argument = parseExpression(0, depth + 1);
            if (!argument.has_value())
                return std::nullopt;
            node.arguments[node.argumentCount++] = *argument;
            if (current_.kind == TokenKind::RightParen)
                break;
            if (current_.kind != TokenKind::Comma)
            {
                addDiagnostic(
                    Diagnostic::Code::ExpectedComma,
                    current_.range,
                    "Expected ',' between series arguments");
                return std::nullopt;
            }
            advance();
            if (current_.kind == TokenKind::RightParen)
            {
                addDiagnostic(
                    Diagnostic::Code::ExpectedExpression,
                    current_.range,
                    "Expected a series argument after ','");
                return std::nullopt;
            }
        }

        if (current_.kind != TokenKind::RightParen)
        {
            addDiagnostic(
                Diagnostic::Code::ExpectedRightParen,
                current_.range,
                "Expected ')' to close series");
            return std::nullopt;
        }
        node.range = joinRanges(node.range, current_.range);
        advance();
        if (node.argumentCount != 3)
        {
            addDiagnostic(
                Diagnostic::Code::InvalidArgumentCount,
                node.range,
                "Pure expression series requires first, step, and count");
            return std::nullopt;
        }
        if (current_.kind != TokenKind::LeftBracket)
        {
            addDiagnostic(
                Diagnostic::Code::UnexpectedToken,
                current_.range,
                "Series must be followed by an index");
            return std::nullopt;
        }
        advance();
        const auto index = parseExpression(0, depth + 1);
        if (!index.has_value())
            return std::nullopt;
        node.arguments[node.argumentCount++] = *index;
        if (current_.kind != TokenKind::RightBracket)
        {
            addDiagnostic(
                Diagnostic::Code::UnexpectedToken,
                current_.range,
                "Expected ']' to close series index");
            return std::nullopt;
        }
        node.range = joinRanges(node.range, current_.range);
        advance();
        return addNode(std::move(node));
    }

    std::optional<NodeId> parseUserCall(
        Token nameToken,
        std::size_t definitionIndex,
        std::size_t depth)
    {
        if (!admitDepth(depth) || definitions_ == nullptr
            || definitionIndex >= definitions_->definitions.size())
        {
            return std::nullopt;
        }

        const auto& definition =
            definitions_->definitions[definitionIndex];
        advance();
        detail::ExpressionNode node;
        node.kind = detail::NodeKind::UserCall;
        node.range = nameToken.range;
        node.referenceIndex = static_cast<std::uint16_t>(definitionIndex);

        while (!budgetExceeded_
               && current_.kind != TokenKind::RightParen)
        {
            if (node.argumentCount >= node.arguments.size())
            {
                addDiagnostic(
                    Diagnostic::Code::InvalidArgumentCount,
                    nameToken.range,
                    "User-defined expression has too many arguments");
                return std::nullopt;
            }
            const auto argument = parseExpression(0, depth + 1);
            if (!argument.has_value())
                return std::nullopt;
            node.arguments[node.argumentCount++] = *argument;
            if (current_.kind == TokenKind::RightParen)
                break;
            if (current_.kind != TokenKind::Comma)
            {
                addDiagnostic(
                    Diagnostic::Code::ExpectedComma,
                    current_.range,
                    "Expected ',' between user-defined arguments");
                return std::nullopt;
            }
            advance();
            if (current_.kind == TokenKind::RightParen)
            {
                addDiagnostic(
                    Diagnostic::Code::ExpectedExpression,
                    current_.range,
                    "Expected a user-defined argument after ','");
                return std::nullopt;
            }
        }

        if (current_.kind != TokenKind::RightParen)
        {
            addDiagnostic(
                Diagnostic::Code::ExpectedRightParen,
                current_.range,
                "Expected ')' to close user-defined expression call");
            return std::nullopt;
        }
        node.range = joinRanges(node.range, current_.range);
        advance();
        if (node.argumentCount != definition.parameters.size())
        {
            addDiagnostic(
                Diagnostic::Code::InvalidArgumentCount,
                node.range,
                "User-defined expression has the wrong number of arguments");
            return std::nullopt;
        }
        return addNode(std::move(node));
    }

    // The MIDI time code received, which the
    // time-code messages compare with their time.
    std::optional<NodeId> midiTimeCodeReference(SourceRange range)
    {
        constexpr std::string_view name = "MIDITimeCode";
        if (!addUniqueReference(
                storage_.eventReferences,
                name,
                ExpressionLimits::maximumEventReferences,
                range,
                "event-reference"))
        {
            return std::nullopt;
        }
        detail::ExpressionNode node;
        node.kind = detail::NodeKind::EventReference;
        node.name = std::string(name);
        node.range = range;
        return addNode(std::move(node));
    }

    // hh:mm:ss.ff SMPTE, seconds at the project's timecode frame rate
    //.
    std::optional<NodeId> parseTimeCodeLiteral()
    {
        const auto timeCodeToken = current_;
        std::array<double, 4> fields{};
        std::size_t field = 0;
        std::size_t start = 0;
        const auto text = timeCodeToken.text;
        for (std::size_t index = 0; index <= text.size(); ++index)
        {
            if (index < text.size() && isAsciiDigit(text[index]))
                continue;
            bool ok = false;
            if (field < fields.size())
                fields[field] = parseDouble(text.substr(start, index - start), ok);
            if (!ok)
            {
                addDiagnostic(Diagnostic::Code::InvalidNumber, timeCodeToken.range,
                    "Invalid time code");
                advance();
                return std::nullopt;
            }
            ++field;
            start = index + 1;
        }
        advance();
        if (field < 3 || current_.kind != TokenKind::Identifier || current_.text != "SMPTE")
        {
            addDiagnostic(Diagnostic::Code::UnknownUnit, timeCodeToken.range,
                "A time code hh:mm:ss.ff should be followed by SMPTE");
            return std::nullopt;
        }
        const auto framesPerSecond = definitions_ != nullptr
            ? static_cast<double>(definitions_->timecodeFramesPerSecond)
            : 30.0;
        detail::ExpressionNode node;
        node.kind = detail::NodeKind::Literal;
        node.number = fields[0] * 3600.0 + fields[1] * 60.0 + fields[2]
            + (field > 3 ? fields[3] / framesPerSecond : 0.0);
        node.unit = Unit::Seconds;
        node.range = joinRanges(timeCodeToken.range, current_.range);
        advance();
        return addNode(std::move(node));
    }

    std::optional<NodeId> parseNumberLiteral()
    {
        if (current_.text.find(':') != std::string_view::npos)
            return parseTimeCodeLiteral();
        const auto numberToken = current_;
        bool ok = false;
        const auto number = parseDouble(numberToken.text, ok);
        if (!ok)
        {
            addDiagnostic(
                Diagnostic::Code::InvalidNumber,
                numberToken.range,
                "Invalid number literal");
            advance();
            return std::nullopt;
        }

        detail::ExpressionNode node;
        node.kind = detail::NodeKind::Literal;
        node.number = number;
        node.range = numberToken.range;
        advance();
        if (current_.kind == TokenKind::Identifier
            && parsePitchName(current_.text).has_value())
        {
            // Pitch notation: an octave and a pitch name, so 4 c is
            // note number 60; sharp or flat then moves it a half step.
            node.number = (number + 1.0) * 12.0 + *parsePitchName(current_.text);
            node.unit = Unit::NoteNumber;
            node.range = joinRanges(node.range, current_.range);
            advance();
            if (current_.kind == TokenKind::Identifier
                && (current_.text == "sharp" || current_.text == "flat"))
            {
                node.number += current_.text == "sharp" ? 1.0 : -1.0;
                node.range = joinRanges(node.range, current_.range);
                advance();
            }
        }
        else if (current_.kind == TokenKind::Identifier)
        {
            const auto unit = parseUnit(current_.text);
            if (!unit.has_value())
            {
                addDiagnostic(
                    Diagnostic::Code::UnknownUnit,
                    current_.range,
                    "Unknown expression unit");
                advance();
            }
            else
            {
                node.unit = *unit;
                node.range = joinRanges(node.range, current_.range);
                advance();
                // A beat at the project's tempo
                // for fields, when it keeps one.
                if (const auto seconds = fieldBeatSeconds(); seconds.has_value() && node.unit == Unit::Beats)
                {
                    node.number *= *seconds;
                    node.unit = Unit::Seconds;
                }
            }
        }
        return addNode(std::move(node));
    }

    [[nodiscard]] static std::optional<BinaryOp> binaryOpFor(
        TokenKind kind) noexcept
    {
        switch (kind)
        {
        case TokenKind::Plus:
            return BinaryOp::Add;
        case TokenKind::Minus:
            return BinaryOp::Subtract;
        case TokenKind::Star:
            return BinaryOp::Multiply;
        case TokenKind::Slash:
            return BinaryOp::Divide;
        case TokenKind::Remainder:
            return BinaryOp::Remainder;
        default:
            return std::nullopt;
        }
    }

    [[nodiscard]] static std::optional<detail::ComparisonOp> comparisonOpFor(
        TokenKind kind) noexcept
    {
        switch (kind)
        {
        case TokenKind::Less:
            return detail::ComparisonOp::Less;
        case TokenKind::LessEqual:
            return detail::ComparisonOp::LessEqual;
        case TokenKind::Greater:
            return detail::ComparisonOp::Greater;
        case TokenKind::GreaterEqual:
            return detail::ComparisonOp::GreaterEqual;
        case TokenKind::EqualEqual:
            return detail::ComparisonOp::Equal;
        case TokenKind::NotEqual:
            return detail::ComparisonOp::NotEqual;
        default:
            return std::nullopt;
        }
    }

    // Above every binary operator and ramp:, so a unary minus takes only
    // its primary and a following ramp: sees the negated receiver.
    static constexpr int unaryOperandBindingPower = 4;

    [[nodiscard]] static int bindingPower(TokenKind kind) noexcept
    {
        switch (kind)
        {
        case TokenKind::Less:
        case TokenKind::LessEqual:
        case TokenKind::Greater:
        case TokenKind::GreaterEqual:
        case TokenKind::EqualEqual:
        case TokenKind::NotEqual:
            return 0;
        case TokenKind::Plus:
        case TokenKind::Minus:
            return 1;
        case TokenKind::Star:
        case TokenKind::Slash:
        case TokenKind::Remainder:
            return 2;
        default:
            return -1;
        }
    }

    std::optional<NodeId> addNode(detail::ExpressionNode node)
    {
        if (isCompiledExpressionLanguage(languageVersion_)
            && storage_.nodes.size() >= ExpressionLimits::maximumNodes)
        {
            failBudget(
                node.range,
                "Expression exceeds the fixed syntax-node budget");
            return std::nullopt;
        }
        storage_.nodes.push_back(std::move(node));
        return static_cast<NodeId>(storage_.nodes.size() - 1);
    }

    [[nodiscard]] static bool isPastedSoundChannel(std::string_view text) noexcept
    {
        return text == "L" || text == "R" || text == "M";
    }

    [[nodiscard]] bool isPlainIdentifier(NodeId id) const noexcept
    {
        return id < storage_.nodes.size()
            && storage_.nodes[id].kind == detail::NodeKind::IdentifierReference
            && storage_.nodes[id].name.find(' ') == std::string::npos;
    }

    // Renames an identifier to the Sound reference it begins, keeping the
    // bare name only while another reference uses it.
    bool nameSoundReference(
        NodeId id,
        std::string_view channel,
        std::optional<double> periodMilliseconds,
        SourceRange range)
    {
        auto& node = storage_.nodes[id];
        const auto sound = node.name;
        node.name = formatSoundReference({sound, channel.front(), periodMilliseconds});
        node.range = range;
        const auto stillUsed = std::ranges::any_of(storage_.nodes, [&](const auto& other) {
            return other.kind == detail::NodeKind::IdentifierReference && other.name == sound;
        });
        if (!stillUsed)
            std::erase(storage_.identifiers, sound);
        return addUniqueReference(storage_.identifiers, node.name,
            ExpressionLimits::maximumIdentifiers, range, "identifier");
    }

    bool addUniqueReference(
        std::vector<std::string>& values,
        std::string_view value,
        std::size_t maximum,
        SourceRange range,
        std::string_view limitName)
    {
        if (std::ranges::find(values, value) != values.end())
            return true;
        if (isCompiledExpressionLanguage(languageVersion_)
            && values.size() >= maximum)
        {
            failBudget(
                range,
                std::string{"Expression exceeds the fixed "}
                    + std::string{limitName} + " limit");
            return false;
        }
        values.emplace_back(value);
        return true;
    }

    [[nodiscard]] bool admitDepth(std::size_t depth)
    {
        if (depth <= (languageVersion_ == ExpressionLanguageVersion::LegacyV1
                    ? ExpressionLimits::maximumLegacyNestingDepth
                    : ExpressionLimits::maximumNestingDepth))
            return true;
        failBudget(
            current_.range,
            "Expression exceeds the fixed nesting budget");
        return false;
    }

    void advance()
    {
        if (budgetExceeded_)
            return;
        if (isCompiledExpressionLanguage(languageVersion_)
            && ++tokenCount_ > ExpressionLimits::maximumTokens)
        {
            failBudget(
                {lexer_.offset(), 0},
                "Expression exceeds the fixed token budget");
            current_ = {TokenKind::End, {}, {lexer_.offset(), 0}};
            return;
        }
        if (languageVersion_ == ExpressionLanguageVersion::LegacyV1)
            ++tokenCount_;
        current_ = lexer_.next();
    }

    void addDiagnostic(
        Diagnostic::Code code,
        SourceRange range,
        std::string message)
    {
        if (budgetExceeded_)
            return;
        if (isCompiledExpressionLanguage(languageVersion_)
            && diagnostics_.size() >= ExpressionLimits::maximumDiagnostics)
        {
            failBudget(
                range,
                "Expression exceeds the fixed diagnostic budget");
            return;
        }
        diagnostics_.push_back(
            makeDiagnostic(code, range, std::move(message)));
    }

    void failBudget(SourceRange range, std::string message)
    {
        if (budgetExceeded_)
            return;
        budgetExceeded_ = true;
        diagnostics_.clear();
        diagnostics_.push_back(makeDiagnostic(
            Diagnostic::Code::BudgetExceeded,
            range,
            std::move(message)));
    }

    void addTrailingDiagnostic()
    {
        if (current_.kind == TokenKind::InvalidCharacter)
        {
            addDiagnostic(
                Diagnostic::Code::UnexpectedCharacter,
                current_.range,
                "Unexpected character in expression");
            return;
        }
        if (current_.kind == TokenKind::Comma)
        {
            addDiagnostic(
                Diagnostic::Code::UnexpectedCharacter,
                current_.range,
                "Unexpected character in expression");
            return;
        }
        if (current_.kind == TokenKind::LeftParen)
        {
            addDiagnostic(
                Diagnostic::Code::UnexpectedToken,
                current_.range,
                "Unexpected '(' after expression");
            return;
        }
        addDiagnostic(
            Diagnostic::Code::TrailingInput,
            current_.range,
            "Unexpected trailing input after expression");
    }

    // Whether `id` holds no units: a plain number or Event Value, or their
    // arithmetic.
    [[nodiscard]] bool unitless(NodeId id) const
    {
        const auto& node = storage_.nodes[id];
        if (node.kind == detail::NodeKind::Literal)
            return node.unit == Unit::None;
        if (node.kind == detail::NodeKind::Binary)
            return unitless(node.left) && unitless(node.right);
        return node.kind == detail::NodeKind::EventReference;
    }

    // Where the bpm `id` is written in ends: a frequency written in bpm, or
    // one a plain number multiplies or divides (30 bpm * 4 is 120 bpm);
    // nothing when `id` is not written in bpm.
    [[nodiscard]] std::optional<std::vector<std::size_t>> bpmWritten(NodeId id) const
    {
        const auto& node = storage_.nodes[id];
        if ((node.kind == detail::NodeKind::UnitApplication || node.kind == detail::NodeKind::Literal)
            && node.unit == Unit::BeatsPerMinute)
        {
            return std::vector<std::size_t>{node.range.offset + node.range.length};
        }
        if (node.kind != detail::NodeKind::Binary
            || (node.binaryOp != BinaryOp::Multiply && node.binaryOp != BinaryOp::Divide))
        {
            return std::nullopt;
        }
        if (unitless(node.right))
        {
            if (auto left = bpmWritten(node.left))
                return left;
        }
        if (node.binaryOp == BinaryOp::Multiply && unitless(node.left))
            return bpmWritten(node.right);
        return std::nullopt;
    }

    ParsedExpression finish(std::optional<NodeId> root)
    {
        ParsedExpression result;
        result.diagnostics = std::move(diagnostics_);
        if (root.has_value() && result.diagnostics.empty())
            result.storage = std::move(storage_);
        return result;
    }

    std::string_view source_;
    Lexer lexer_;
    ExpressionLanguageVersion languageVersion_;
    const ExpressionDefinitionLibrary* definitions_ = nullptr;
    const std::vector<ExpressionDefinitionParameter>* formals_ = nullptr;
    std::vector<std::size_t>* bpmFactorEnds_ = nullptr;
    std::vector<SourceRange>* metronomeRanges_ = nullptr;
    std::vector<SourceRange>* signSelectionRanges_ = nullptr;
    bool wholeSoundClocks_ = false;
    Token current_;
    detail::ExpressionStorage storage_;
    std::vector<Diagnostic> diagnostics_;
    std::size_t tokenCount_ = 0;
    bool budgetExceeded_ = false;
};

[[nodiscard]] bool isValidDefinitionIdentifier(
    std::string_view value) noexcept
{
    if (value.empty()
        || value.size() > ExpressionLimits::maximumDefinitionNameBytes
        || !isIdentifierStart(value.front()))
    {
        return false;
    }
    return std::ranges::all_of(
        value.substr(1),
        [](char character) { return isIdentifierContinue(character); });
}

[[nodiscard]] bool isSupportedValueDomain(ValueDomain domain) noexcept
{
    switch (domain)
    {
    case ValueDomain::Scalar:
    case ValueDomain::Hertz:
    case ValueDomain::Seconds:
    case ValueDomain::NoteNumber:
    case ValueDomain::Samples:
        return true;
    }
    return false;
}

[[nodiscard]] bool validateDefinitionLibrary(
    ExpressionLanguageVersion languageVersion,
    const ExpressionDefinitionLibrary& library,
    Diagnostic& failure)
{
    if (!supportsExpressionDefinitions(languageVersion))
    {
        if (library.definitions.empty())
            return true;
        failure = makeDiagnostic(
            Diagnostic::Code::InvalidDefinition,
            {},
            "User-defined expressions require PureV4 or SessionV1");
        return false;
    }
    if (library.definitions.size()
        > ExpressionLimits::maximumDefinitions)
    {
        failure = makeDiagnostic(
            Diagnostic::Code::BudgetExceeded,
            {},
            "Expression definition library exceeds its fixed count budget");
        return false;
    }

    std::size_t sourceBytes = 0;
    std::array<std::vector<std::size_t>,
               ExpressionLimits::maximumDefinitions> edges;
    for (std::size_t definitionIndex = 0;
         definitionIndex < library.definitions.size();
         ++definitionIndex)
    {
        const auto& definition = library.definitions[definitionIndex];
        if (!isValidDefinitionIdentifier(definition.name)
            || definition.name == "series"
            || (definition.name == "intoXArray"
                && supportsArrayMap(languageVersion))
            || availableFunctionFor(
                   definition.name,
                   languageVersion).has_value()
            || !isSupportedValueDomain(definition.resultDomain))
        {
            failure = makeDiagnostic(
                Diagnostic::Code::InvalidDefinition,
                {},
                "Expression definition has an invalid or reserved name or result domain");
            failure.definitionIndex = definitionIndex;
            return false;
        }
        if (std::ranges::any_of(
                library.definitions.begin(),
                library.definitions.begin()
                    + static_cast<std::ptrdiff_t>(definitionIndex),
                [&definition](const ExpressionDefinition& prior) {
                    return prior.name == definition.name;
                }))
        {
            failure = makeDiagnostic(
                Diagnostic::Code::InvalidDefinition,
                {},
                "Expression definition names must be unique");
            failure.definitionIndex = definitionIndex;
            return false;
        }
        if (definition.parameters.size()
            > ExpressionLimits::maximumDefinitionParameters)
        {
            failure = makeDiagnostic(
                Diagnostic::Code::BudgetExceeded,
                {},
                "Expression definition exceeds its fixed parameter budget");
            failure.definitionIndex = definitionIndex;
            return false;
        }
        for (std::size_t parameterIndex = 0;
             parameterIndex < definition.parameters.size();
             ++parameterIndex)
        {
            const auto& parameter = definition.parameters[parameterIndex];
            if (!isValidDefinitionIdentifier(parameter.name)
                || parameter.name == "series"
                || (parameter.name == "intoXArray"
                    && supportsArrayMap(languageVersion))
                || availableFunctionFor(
                       parameter.name,
                       languageVersion).has_value()
                || !isSupportedValueDomain(parameter.domain)
                || std::ranges::any_of(
                    definition.parameters.begin(),
                    definition.parameters.begin()
                        + static_cast<std::ptrdiff_t>(parameterIndex),
                    [&parameter](
                        const ExpressionDefinitionParameter& prior) {
                        return prior.name == parameter.name;
                    }))
            {
                failure = makeDiagnostic(
                    Diagnostic::Code::InvalidDefinition,
                    {},
                    "Expression definition parameters must have unique valid names and domains");
                failure.definitionIndex = definitionIndex;
                return false;
            }
        }
        if (definition.source.size()
            > ExpressionLimits::maximumDefinitionSourceBytes
            || sourceBytes
                > ExpressionLimits::maximumDefinitionSourceBytesTotal
                    - definition.source.size())
        {
            failure = makeDiagnostic(
                Diagnostic::Code::BudgetExceeded,
                {},
                "Expression definition sources exceed their fixed byte budget");
            failure.definitionIndex = definitionIndex;
            return false;
        }
        sourceBytes += definition.source.size();

        auto parsed = Parser(
            definition.source,
            languageVersion,
            &library,
            &definition.parameters).parse();
        if (!parsed.storage.has_value())
        {
            failure = parsed.diagnostics.empty()
                ? makeDiagnostic(
                    Diagnostic::Code::InvalidDefinition,
                    {},
                    "Expression definition body is invalid")
                : std::move(parsed.diagnostics.front());
            failure.definitionIndex = definitionIndex;
            return false;
        }
        for (const auto& node : parsed.storage->nodes)
        {
            if (node.kind == detail::NodeKind::UserCall)
                edges[definitionIndex].push_back(node.referenceIndex);
        }
    }

    enum class Visit : std::uint8_t { Unseen, Active, Complete };
    std::array<Visit, ExpressionLimits::maximumDefinitions> visits{};
    std::array<std::size_t, ExpressionLimits::maximumDefinitions> depths{};
    const auto visit = [&](auto&& self,
                           std::size_t definitionIndex) -> bool {
        if (visits[definitionIndex] == Visit::Active)
        {
            failure = makeDiagnostic(
                Diagnostic::Code::RecursiveDefinition,
                {},
                "Recursive expression definitions are not allowed");
            failure.definitionIndex = definitionIndex;
            return false;
        }
        if (visits[definitionIndex] == Visit::Complete)
            return true;
        visits[definitionIndex] = Visit::Active;
        auto maximumChildDepth = std::size_t{0};
        for (const auto target : edges[definitionIndex])
        {
            if (target >= library.definitions.size()
                || !self(self, target))
            {
                return false;
            }
            maximumChildDepth = std::max(
                maximumChildDepth,
                depths[target]);
        }
        depths[definitionIndex] = 1 + maximumChildDepth;
        if (depths[definitionIndex]
            > ExpressionLimits::maximumDefinitionExpansionDepth)
        {
            failure = makeDiagnostic(
                Diagnostic::Code::BudgetExceeded,
                {},
                "Expression definition expansion exceeds its fixed depth budget");
            failure.definitionIndex = definitionIndex;
            return false;
        }
        visits[definitionIndex] = Visit::Complete;
        return true;
    };
    for (std::size_t index = 0; index < library.definitions.size(); ++index)
    {
        if (!visit(visit, index))
            return false;
    }
    return true;
}

[[nodiscard]] detail::InstructionOp instructionFor(
    BinaryOp operation) noexcept
{
    switch (operation)
    {
    case BinaryOp::Add:
        return detail::InstructionOp::Add;
    case BinaryOp::Subtract:
        return detail::InstructionOp::Subtract;
    case BinaryOp::Multiply:
        return detail::InstructionOp::Multiply;
    case BinaryOp::Divide:
        return detail::InstructionOp::Divide;
    case BinaryOp::Remainder:
        return detail::InstructionOp::Remainder;
    case BinaryOp::Power:
        return detail::InstructionOp::Power;
    case BinaryOp::IntegerDivide:
        return detail::InstructionOp::IntegerDivide;
    case BinaryOp::Modulo:
        return detail::InstructionOp::Modulo;
    }
    return detail::InstructionOp::Add;
}

[[nodiscard]] detail::InstructionOp instructionFor(
    detail::ComparisonOp operation) noexcept
{
    switch (operation)
    {
    case detail::ComparisonOp::Less:
        return detail::InstructionOp::Less;
    case detail::ComparisonOp::LessEqual:
        return detail::InstructionOp::LessEqual;
    case detail::ComparisonOp::Greater:
        return detail::InstructionOp::Greater;
    case detail::ComparisonOp::GreaterEqual:
        return detail::InstructionOp::GreaterEqual;
    case detail::ComparisonOp::Equal:
        return detail::InstructionOp::Equal;
    case detail::ComparisonOp::NotEqual:
        return detail::InstructionOp::NotEqual;
    }
    return detail::InstructionOp::Equal;
}

[[nodiscard]] detail::InstructionOp instructionFor(
    detail::FunctionOp function) noexcept
{
    switch (function)
    {
    case detail::FunctionOp::Abs:
        return detail::InstructionOp::Abs;
    case detail::FunctionOp::Sqrt:
        return detail::InstructionOp::Sqrt;
    case detail::FunctionOp::Log:
        return detail::InstructionOp::Log;
    case detail::FunctionOp::Exp:
        return detail::InstructionOp::Exp;
    case detail::FunctionOp::Clamp:
        return detail::InstructionOp::Clamp;
    case detail::FunctionOp::Map:
        return detail::InstructionOp::Map;
    case detail::FunctionOp::If:
        return detail::InstructionOp::JumpIfFalse;
    case detail::FunctionOp::Previous:
        return detail::InstructionOp::Previous;
    case detail::FunctionOp::Accumulate:
        return detail::InstructionOp::Accumulate;
    case detail::FunctionOp::SampleHold:
        return detail::InstructionOp::SampleHold;
    case detail::FunctionOp::Ramp:
        return detail::InstructionOp::Ramp;
    case detail::FunctionOp::Log10:
        return detail::InstructionOp::Log10;
    case detail::FunctionOp::Log2:
        return detail::InstructionOp::Log2;
    case detail::FunctionOp::Exp2:
        return detail::InstructionOp::Exp2;
    case detail::FunctionOp::Squared:
        return detail::InstructionOp::Squared;
    case detail::FunctionOp::Reciprocal:
        return detail::InstructionOp::Reciprocal;
    case detail::FunctionOp::Sign:
        return detail::InstructionOp::Sign;
    case detail::FunctionOp::Truncate:
        return detail::InstructionOp::Truncate;
    case detail::FunctionOp::Round:
        return detail::InstructionOp::Round;
    case detail::FunctionOp::ClipTo01:
        return detail::InstructionOp::ClipTo01;
    case detail::FunctionOp::WrapTo02:
        return detail::InstructionOp::WrapTo02;
    case detail::FunctionOp::Cos:
        return detail::InstructionOp::Cos;
    case detail::FunctionOp::Sin:
        return detail::InstructionOp::Sin;
    case detail::FunctionOp::NormCos:
        return detail::InstructionOp::NormCos;
    case detail::FunctionOp::NormSin:
        return detail::InstructionOp::NormSin;
    case detail::FunctionOp::LogicValue:
        return detail::InstructionOp::LogicValue;
    case detail::FunctionOp::RemoveUnits:
        return detail::InstructionOp::RemoveUnits;
    case detail::FunctionOp::Min:
        return detail::InstructionOp::Min;
    case detail::FunctionOp::Max:
        return detail::InstructionOp::Max;
    case detail::FunctionOp::RoundTo:
        return detail::InstructionOp::RoundTo;
    case detail::FunctionOp::ArrayIndex:
        return detail::InstructionOp::ArrayIndex;
    case detail::FunctionOp::Floor:
        return detail::InstructionOp::Floor;
    case detail::FunctionOp::Ceiling:
        return detail::InstructionOp::Ceiling;
    case detail::FunctionOp::RoundHalfUp:
        return detail::InstructionOp::RoundHalfUp;
    case detail::FunctionOp::LinearUnitlessSOSPitch:
        return detail::InstructionOp::LinearUnitlessSOSPitch;
    case detail::FunctionOp::ArcTan:
        return detail::InstructionOp::ArcTan;
    case detail::FunctionOp::ArcSin:
        return detail::InstructionOp::ArcSin;
    case detail::FunctionOp::ArcCos:
        return detail::InstructionOp::ArcCos;
    case detail::FunctionOp::Tan:
        return detail::InstructionOp::Tan;
    case detail::FunctionOp::Sinc:
        return detail::InstructionOp::Sinc;
    case detail::FunctionOp::TruncateTo:
        return detail::InstructionOp::TruncateTo;
    case detail::FunctionOp::CeilingTo:
        return detail::InstructionOp::CeilingTo;
    case detail::FunctionOp::MaxAbs:
        return detail::InstructionOp::MaxAbs;
    case detail::FunctionOp::MinAbs:
        return detail::InstructionOp::MinAbs;
    case detail::FunctionOp::LogBase:
        return detail::InstructionOp::LogBase;
    case detail::FunctionOp::FloorLog:
        return detail::InstructionOp::FloorLog;
    case detail::FunctionOp::Between:
        return detail::InstructionOp::Between;
    case detail::FunctionOp::TimeFunction:
        return detail::InstructionOp::TimeFunction;
    case detail::FunctionOp::Metronome:
        return detail::InstructionOp::Metronome;
    case detail::FunctionOp::RandomInterval:
        return detail::InstructionOp::RandomInterval;
    case detail::FunctionOp::NextRandom:
        return detail::InstructionOp::NextRandom;
    case detail::FunctionOp::Smooth:
        return detail::InstructionOp::Smooth;
    case detail::FunctionOp::CountTriggers:
        return detail::InstructionOp::CountTriggers;
    case detail::FunctionOp::TriggerEvery:
        return detail::InstructionOp::TriggerEvery;
    case detail::FunctionOp::Toggle:
        return detail::InstructionOp::Toggle;
    case detail::FunctionOp::GateWhen:
        return detail::InstructionOp::GateWhen;
    case detail::FunctionOp::TriggerDuration:
        return detail::InstructionOp::TriggerDuration;
    case detail::FunctionOp::ChangedReset:
        return detail::InstructionOp::ChangedReset;
    case detail::FunctionOp::SustainBy:
        return detail::InstructionOp::SustainBy;
    case detail::FunctionOp::NextRandomSeeded:
        return detail::InstructionOp::NextRandomSeeded;
    case detail::FunctionOp::Chaotic:
        return detail::InstructionOp::Chaotic;
    case detail::FunctionOp::TrackHold:
        return detail::InstructionOp::TrackHold;
    case detail::FunctionOp::Initially:
        return detail::InstructionOp::Initially;
    case detail::FunctionOp::SwarmFollow:
        return detail::InstructionOp::SwarmFollow;
    case detail::FunctionOp::Recurrence:
        return detail::InstructionOp::RecurrenceStore;
    case detail::FunctionOp::EventVariableLoad:
        return detail::InstructionOp::EventVariableLoad;
    case detail::FunctionOp::EventVariableStore:
        return detail::InstructionOp::EventVariableStore;
    case detail::FunctionOp::Sequence:
        return detail::InstructionOp::Sequence;
    case detail::FunctionOp::MidiTimingClock:
        return detail::InstructionOp::MidiTimingClock;
    case detail::FunctionOp::Shared:
    case detail::FunctionOp::UpdateInternalState:
    case detail::FunctionOp::EveryMillisecond:
    case detail::FunctionOp::Nothing:
        break;
    }
    return detail::InstructionOp::Abs;
}

// Whether evaluating `id` leaves no value on Val's stack: an assignment with <+, nil, a time function updated for its
// state alone, a variable read every millisecond, or a sequence of those.
[[nodiscard]] bool leavesNothing(const detail::ExpressionStorage& storage, NodeId id) noexcept
{
    const auto& node = storage.nodes[id];
    if (node.kind != detail::NodeKind::Call)
        return false;
    switch (node.functionOp)
    {
    case detail::FunctionOp::EventVariableStore:
        return node.number == 0.0;
    case detail::FunctionOp::Nothing:
    case detail::FunctionOp::UpdateInternalState:
    case detail::FunctionOp::EveryMillisecond:
        return true;
    case detail::FunctionOp::Sequence:
        return leavesNothing(storage, node.arguments[0]) && leavesNothing(storage, node.arguments[1]);
    default:
        return false;
    }
}

class Compiler
{
public:
    Compiler(
        detail::ExpressionStorage& storage,
        const ExpressionDefinitionLibrary* definitions = nullptr)
        : storage_(storage), definitions_(definitions)
    {
    }

    [[nodiscard]] bool compile(Diagnostic& failure)
    {
        storage_.nodes.reserve(ExpressionLimits::maximumNodes);
        storage_.listElements.reserve(ExpressionLimits::maximumNodes);
        storage_.eventReferences.reserve(
            ExpressionLimits::maximumEventReferences);
        storage_.variableReferences.reserve(
            ExpressionLimits::maximumVariableReferences);
        storage_.variableReferenceMetadata.reserve(
            ExpressionLimits::maximumInstructions);
        storage_.identifiers.reserve(ExpressionLimits::maximumIdentifiers);
        // EventVariables take the first state cells, each by its number, and
        // their program one more holding the value it computed in its
        // millisecond.
        std::size_t variables = 0;
        for (const auto& node : storage_.nodes)
        {
            if (node.kind != detail::NodeKind::Call || node.functionOp != detail::FunctionOp::EventVariableLoad)
                continue;
            const auto& number = storage_.nodes[node.arguments[0]];
            if (number.kind != detail::NodeKind::Literal || number.number < 0.0
                || number.number != std::floor(number.number)
                || number.number + 2.0 > static_cast<double>(ExpressionLimits::maximumStateSlots)
                || !supportsStatefulExpressions(storage_.languageVersion))
            {
                failure = makeDiagnostic(Diagnostic::Code::BudgetExceeded, node.range,
                    "Expression exceeds or does not support its EventVariables");
                return false;
            }
            variables = std::max(variables, static_cast<std::size_t>(number.number) + 1);
        }
        std::optional<std::size_t> gate;
        if (variables > 0)
        {
            nextStateSlot_ = variables + 1;
            gate = storage_.instructions.size();
            detail::Instruction begin;
            begin.op = detail::InstructionOp::GateBegin;
            begin.range = storage_.nodes[storage_.root].range;
            begin.referenceIndex = static_cast<std::uint16_t>(variables);
            if (!append(begin, failure))
                return false;
        }
        std::size_t depth = 0;
        if (!compileNode(storage_.root, depth, failure))
            return false;
        if (gate.has_value())
        {
            detail::Instruction end;
            end.op = detail::InstructionOp::GateEnd;
            end.range = storage_.nodes[storage_.root].range;
            end.referenceIndex = static_cast<std::uint16_t>(variables);
            if (!append(end, failure)
                || storage_.instructions.size() > std::numeric_limits<std::uint16_t>::max())
                return false;
            storage_.instructions[*gate].target = static_cast<std::uint16_t>(storage_.instructions.size());
        }
        storage_.maximumStackDepth = depth;
        storage_.localSlotCount = nextLocalSlot_;
        storage_.stateSlotCount = nextStateSlot_;
        if (depth == 0
            || depth > ExpressionLimits::maximumEvaluationStack)
        {
            failure = makeDiagnostic(
                Diagnostic::Code::BudgetExceeded,
                storage_.nodes[storage_.root].range,
                "Expression exceeds the fixed evaluation-stack budget");
            return false;
        }
        return true;
    }

private:
    [[nodiscard]] bool append(
        detail::Instruction instruction,
        Diagnostic& failure)
    {
        if (!instruction.definitionIndex.has_value()
            && currentDefinitionIndex_.has_value())
        {
            instruction.definitionIndex = currentDefinitionIndex_;
        }
        if (storage_.instructions.size()
            >= ExpressionLimits::maximumInstructions)
        {
            failure = makeDiagnostic(
                Diagnostic::Code::BudgetExceeded,
                instruction.range,
                "Expression exceeds the fixed instruction budget");
            failure.definitionIndex = instruction.definitionIndex;
            return false;
        }
        if (!storage_.voiceNumberInstruction.has_value()
            && instruction.op == detail::InstructionOp::ResolveIdentifier
            && instruction.referenceIndex < storage_.identifiers.size()
            && storage_.identifiers[instruction.referenceIndex] == voiceNumberIdentifier)
            storage_.voiceNumberInstruction = storage_.instructions.size();
        storage_.instructions.push_back(instruction);
        return true;
    }

    [[nodiscard]] std::optional<std::uint16_t> ensureReferenceIndex(
        std::vector<std::string>& references,
        std::string_view name,
        std::size_t maximum,
        Diagnostic& failure)
    {
        const auto found = std::ranges::find(references, name);
        if (found != references.end())
        {
            return static_cast<std::uint16_t>(
                static_cast<std::size_t>(found - references.begin()));
        }
        if (references.size() >= maximum)
        {
            failure = makeDiagnostic(
                Diagnostic::Code::BudgetExceeded,
                {},
                "Expression expansion exceeds its fixed reference budget");
            return std::nullopt;
        }
        references.emplace_back(name);
        return static_cast<std::uint16_t>(
            references.size() - 1);
    }

    [[nodiscard]] std::optional<NodeId> importDefinitionBody(
        const ExpressionDefinition& definition,
        const std::vector<std::uint16_t>& localSlots,
        Diagnostic& failure)
    {
        auto parsed = Parser(
            definition.source,
            storage_.languageVersion,
            definitions_,
            &definition.parameters).parse();
        if (!parsed.storage.has_value())
        {
            failure = parsed.diagnostics.empty()
                ? makeDiagnostic(
                    Diagnostic::Code::InvalidDefinition,
                    {},
                    "Expression definition body is invalid")
                : std::move(parsed.diagnostics.front());
            return std::nullopt;
        }
        const auto& source = *parsed.storage;
        if (source.nodes.size()
                > ExpressionLimits::maximumNodes - storage_.nodes.size()
            || source.listElements.size()
                > ExpressionLimits::maximumNodes
                    - storage_.listElements.size())
        {
            failure = makeDiagnostic(
                Diagnostic::Code::BudgetExceeded,
                {},
                "Expression definition expansion exceeds its fixed node budget");
            return std::nullopt;
        }

        for (const auto& reference : source.eventReferences)
        {
            if (!ensureReferenceIndex(
                    storage_.eventReferences,
                    reference,
                    ExpressionLimits::maximumEventReferences,
                    failure).has_value())
            {
                return std::nullopt;
            }
        }
        for (const auto& reference : source.variableReferences)
        {
            if (!ensureReferenceIndex(
                    storage_.variableReferences,
                    reference,
                    ExpressionLimits::maximumVariableReferences,
                    failure).has_value())
            {
                return std::nullopt;
            }
        }
        for (const auto& reference : source.identifiers)
        {
            if (!ensureReferenceIndex(
                    storage_.identifiers,
                    reference,
                    ExpressionLimits::maximumIdentifiers,
                    failure).has_value())
            {
                return std::nullopt;
            }
        }

        const auto nodeOffset = storage_.nodes.size();
        const auto listOffset = storage_.listElements.size();
        for (const auto id : source.listElements)
            storage_.listElements.push_back(static_cast<NodeId>(nodeOffset + id));
        storage_.nodes.reserve(storage_.nodes.size() + source.nodes.size());
        for (auto node : source.nodes)
        {
            const auto adjust = [nodeOffset](NodeId id) {
                return static_cast<NodeId>(nodeOffset + id);
            };
            switch (node.kind)
            {
            case detail::NodeKind::Unary:
            case detail::NodeKind::UnitApplication:
                node.left = adjust(node.left);
                break;
            case detail::NodeKind::Binary:
            case detail::NodeKind::Comparison:
                node.left = adjust(node.left);
                node.right = adjust(node.right);
                break;
            case detail::NodeKind::ListIndex:
                node.left = adjust(node.left);
                node.listElementOffset = static_cast<std::uint16_t>(
                    listOffset + node.listElementOffset);
                break;
            case detail::NodeKind::ArrayMap:
                node.left = adjust(node.left);
                node.right = adjust(node.right);
                node.listElementOffset = static_cast<std::uint16_t>(
                    listOffset + node.listElementOffset);
                break;
            case detail::NodeKind::Call:
            case detail::NodeKind::SeriesIndex:
            case detail::NodeKind::UserCall:
                for (std::size_t index = 0;
                     index < node.argumentCount;
                     ++index)
                {
                    node.arguments[index] = adjust(node.arguments[index]);
                }
                break;
            case detail::NodeKind::FormalReference:
                if (node.referenceIndex >= localSlots.size())
                {
                    failure = makeDiagnostic(
                        Diagnostic::Code::BudgetExceeded,
                        node.range,
                        "Expression definition has an invalid formal reference");
                    return std::nullopt;
                }
                node.referenceIndex = localSlots[node.referenceIndex];
                break;
            case detail::NodeKind::Literal:
            case detail::NodeKind::EventReference:
            case detail::NodeKind::VariableReference:
            case detail::NodeKind::IdentifierReference:
                break;
            }
            storage_.nodes.push_back(std::move(node));
        }
        return static_cast<NodeId>(nodeOffset + source.root);
    }

    [[nodiscard]] bool compileNode(
        NodeId id,
        std::size_t& maximumDepth,
        Diagnostic& failure)
    {
        if (id >= storage_.nodes.size())
            return false;
        const auto& node = storage_.nodes[id];
        switch (node.kind)
        {
        case detail::NodeKind::Literal:
            maximumDepth = 1;
            return append(
                {detail::InstructionOp::PushLiteral,
                 node.range,
                 node.number,
                 node.unit},
                failure);
        case detail::NodeKind::EventReference:
        case detail::NodeKind::VariableReference:
        case detail::NodeKind::IdentifierReference:
        {
            if (node.kind == detail::NodeKind::IdentifierReference && !recurrenceLocals_.empty()
                && node.name == "talasRecurrentValue")
            {
                maximumDepth = 1;
                detail::Instruction instruction;
                instruction.op = detail::InstructionOp::LoadLocal;
                instruction.range = node.range;
                instruction.referenceIndex = recurrenceLocals_.back();
                return append(instruction, failure);
            }
            const auto kind = node.kind == detail::NodeKind::EventReference
                ? ReferenceKind::Event
                : node.kind == detail::NodeKind::VariableReference
                    ? ReferenceKind::Variable
                    : ReferenceKind::Identifier;
            auto& references = kind == ReferenceKind::Event
                ? storage_.eventReferences
                : kind == ReferenceKind::Variable
                    ? storage_.variableReferences
                    : storage_.identifiers;
            const auto index = ensureReferenceIndex(
                references,
                node.name,
                kind == ReferenceKind::Event
                    ? ExpressionLimits::maximumEventReferences
                    : kind == ReferenceKind::Variable
                        ? ExpressionLimits::maximumVariableReferences
                        : ExpressionLimits::maximumIdentifiers,
                failure);
            if (!index.has_value())
                return false;
            maximumDepth = 1;
            detail::Instruction instruction;
            instruction.op = kind == ReferenceKind::Event
                ? detail::InstructionOp::ResolveEvent
                : kind == ReferenceKind::Variable
                    ? detail::InstructionOp::ResolveVariable
                    : detail::InstructionOp::ResolveIdentifier;
            instruction.range = node.range;
            instruction.referenceIndex = *index;
            if (!append(instruction, failure))
                return false;
            if (kind == ReferenceKind::Variable)
            {
                storage_.variableReferenceMetadata.push_back({
                    node.name,
                    node.range,
                    storage_.instructions.back().definitionIndex,
                });
            }
            return true;
        }
        case detail::NodeKind::FormalReference:
        {
            if (node.referenceIndex
                >= ExpressionLimits::maximumDefinitionLocals)
            {
                failure = makeDiagnostic(
                    Diagnostic::Code::BudgetExceeded,
                    node.range,
                    "Expression definition local is outside its fixed budget");
                return false;
            }
            maximumDepth = 1;
            detail::Instruction instruction;
            instruction.op = detail::InstructionOp::LoadLocal;
            instruction.range = node.range;
            instruction.referenceIndex = node.referenceIndex;
            return append(instruction, failure);
        }
        case detail::NodeKind::Unary:
        case detail::NodeKind::UnitApplication:
        {
            if (!compileNode(node.left, maximumDepth, failure))
                return false;
            return append(
                {node.kind == detail::NodeKind::UnitApplication
                    ? detail::InstructionOp::ApplyUnit : detail::InstructionOp::Negate,
                 node.range, 0.0, node.unit},
                failure);
        }
        case detail::NodeKind::Binary:
        case detail::NodeKind::Comparison:
        {
            std::size_t leftDepth = 0;
            std::size_t rightDepth = 0;
            if (!compileNode(node.left, leftDepth, failure)
                || !compileNode(node.right, rightDepth, failure))
            {
                return false;
            }
            maximumDepth = std::max(leftDepth, 1 + rightDepth);
            const auto operation = node.kind == detail::NodeKind::Binary
                ? instructionFor(node.binaryOp)
                : instructionFor(node.comparisonOp);
            return append({operation, node.range}, failure);
        }
        case detail::NodeKind::ListIndex:
        {
            const auto elementOffset =
                static_cast<std::size_t>(node.listElementOffset);
            const auto elementCount =
                static_cast<std::size_t>(node.listElementCount);
            if (elementCount == 0
                || elementCount > ExpressionLimits::maximumListElements
                || elementOffset > storage_.listElements.size()
                || elementCount
                    > storage_.listElements.size() - elementOffset
                || storage_.listDispatches.size()
                    >= ExpressionLimits::maximumNodes)
            {
                failure = makeDiagnostic(
                    Diagnostic::Code::BudgetExceeded,
                    node.range,
                    "Expression has an invalid compiled list dispatch");
                return false;
            }

            // Each element runs and is kept in a local, then the index
            // picks one.
            if (node.number > 0.0)
            {
                if (elementCount > ExpressionLimits::maximumDefinitionLocals - nextLocalSlot_)
                {
                    failure = makeDiagnostic(Diagnostic::Code::BudgetExceeded, node.range,
                        "An Array whose elements all run holds at most its fixed local budget");
                    return false;
                }
                const auto base = nextLocalSlot_;
                nextLocalSlot_ += elementCount;
                maximumDepth = 0;
                for (std::size_t index = 0; index < elementCount; ++index)
                {
                    std::size_t elementDepth = 0;
                    if (!compileNode(storage_.listElements[elementOffset + index], elementDepth, failure))
                        return false;
                    maximumDepth = std::max(maximumDepth, elementDepth);
                    detail::Instruction store;
                    store.op = detail::InstructionOp::StoreLocal;
                    store.range = storage_.nodes[storage_.listElements[elementOffset + index]].range;
                    store.referenceIndex = static_cast<std::uint16_t>(base + index);
                    // Kept in its own domain.
                    store.number = 1.0;
                    if (!append(store, failure))
                        return false;
                }
                std::size_t indexDepth = 0;
                if (!compileNode(node.left, indexDepth, failure))
                    return false;
                detail::Instruction pick;
                pick.op = detail::InstructionOp::PickLocal;
                pick.range = storage_.nodes[node.left].range;
                pick.referenceIndex = static_cast<std::uint16_t>(base);
                pick.target = static_cast<std::uint16_t>(elementCount);
                maximumDepth = std::max(maximumDepth, indexDepth);
                return append(pick, failure);
            }
            std::size_t indexDepth = 0;
            if (!compileNode(node.left, indexDepth, failure))
                return false;
            const auto dispatchIndex = storage_.listDispatches.size();
            detail::ListDispatch dispatch;
            dispatch.elementCount = node.listElementCount;
            storage_.listDispatches.push_back(dispatch);
            detail::Instruction select;
            select.op = detail::InstructionOp::SelectList;
            select.range = storage_.nodes[node.left].range;
            select.referenceIndex =
                static_cast<std::uint16_t>(dispatchIndex);
            if (!append(select, failure))
                return false;

            std::array<std::size_t,
                       ExpressionLimits::maximumListElements> jumps{};
            std::size_t branchDepth = 0;
            for (std::size_t index = 0; index < elementCount; ++index)
            {
                const auto target = storage_.instructions.size();
                if (target > std::numeric_limits<std::uint16_t>::max())
                    return false;
                storage_.listDispatches[dispatchIndex].targets[index] =
                    static_cast<std::uint16_t>(target);
                std::size_t elementDepth = 0;
                if (!compileNode(
                        storage_.listElements[elementOffset + index],
                        elementDepth,
                        failure))
                {
                    return false;
                }
                branchDepth = std::max(branchDepth, elementDepth);
                jumps[index] = storage_.instructions.size();
                if (!append(
                        {detail::InstructionOp::Jump, node.range},
                        failure))
                {
                    return false;
                }
            }
            const auto endTarget = storage_.instructions.size();
            if (endTarget > std::numeric_limits<std::uint16_t>::max())
                return false;
            for (std::size_t index = 0; index < elementCount; ++index)
            {
                storage_.instructions[jumps[index]].target =
                    static_cast<std::uint16_t>(endTarget);
            }
            maximumDepth = std::max(indexDepth, branchDepth);
            return true;
        }
        case detail::NodeKind::ArrayMap:
        {
            const auto count = static_cast<std::size_t>(node.listElementCount);
            const auto offset = static_cast<std::size_t>(node.listElementOffset);
            const auto requiredSlots = 2 + 2 * count;
            if (count < 2 || count > 15
                || offset > storage_.listElements.size()
                || 2 * count > storage_.listElements.size() - offset
                || requiredSlots
                    > ExpressionLimits::maximumDefinitionLocals - nextLocalSlot_)
            {
                failure = makeDiagnostic(
                    Diagnostic::Code::BudgetExceeded,
                    node.range,
                    "intoXArray exceeds its fixed point or local budget");
                return false;
            }
            const auto base = nextLocalSlot_;
            nextLocalSlot_ += requiredSlots;
            maximumDepth = 0;
            const auto compileScalar = [&](NodeId valueId, std::size_t slot) {
                std::size_t depth = 0;
                if (!compileNode(valueId, depth, failure))
                    return false;
                maximumDepth = std::max(maximumDepth, depth);
                detail::Instruction store;
                store.op = detail::InstructionOp::StoreLocal;
                store.range = storage_.nodes[valueId].range;
                store.referenceIndex = static_cast<std::uint16_t>(slot);
                store.domain = ValueDomain::Scalar;
                return append(store, failure);
            };
            if (!compileScalar(node.left, base)
                || !compileScalar(node.right, base + 1))
            {
                return false;
            }
            for (std::size_t index = 0; index < 2 * count; ++index)
            {
                if (!compileScalar(
                        storage_.listElements[offset + index],
                        base + 2 + index))
                {
                    return false;
                }
            }
            detail::Instruction map;
            map.op = detail::InstructionOp::ArrayMap;
            map.range = node.range;
            map.referenceIndex = static_cast<std::uint16_t>(base);
            map.target = static_cast<std::uint16_t>(count);
            maximumDepth = std::max<std::size_t>(maximumDepth, 1);
            return append(map, failure);
        }
        case detail::NodeKind::SeriesIndex:
        {
            if (node.argumentCount != 4)
            {
                failure = makeDiagnostic(
                    Diagnostic::Code::BudgetExceeded,
                    node.range,
                    "Expression has an invalid compiled series");
                return false;
            }

            const auto first = node.arguments[0];
            const auto step = node.arguments[1];
            const auto count = node.arguments[2];
            const auto index = node.arguments[3];
            std::size_t indexDepth = 0;
            if (!compileNode(index, indexDepth, failure)
                || !append(
                    {detail::InstructionOp::ValidateSequenceIndex,
                     storage_.nodes[index].range},
                    failure))
            {
                return false;
            }

            std::size_t countDepth = 0;
            if (!compileNode(count, countDepth, failure)
                || !append(
                    {detail::InstructionOp::ValidateSequenceCount,
                     storage_.nodes[count].range},
                    failure)
                || !append(
                    {detail::InstructionOp::CheckSequenceBounds,
                     storage_.nodes[index].range},
                    failure))
            {
                return false;
            }

            std::size_t firstDepth = 0;
            std::size_t stepDepth = 0;
            if (!compileNode(first, firstDepth, failure)
                || !compileNode(step, stepDepth, failure)
                || !append(
                    {detail::InstructionOp::EvaluateSeries, node.range},
                    failure))
            {
                return false;
            }
            maximumDepth = std::max(
                {indexDepth,
                 1 + countDepth,
                 1 + firstDepth,
                 2 + stepDepth});
            return true;
        }
        case detail::NodeKind::UserCall:
        {
            if (definitions_ == nullptr
                || node.referenceIndex >= definitions_->definitions.size())
            {
                failure = makeDiagnostic(
                    Diagnostic::Code::InvalidDefinition,
                    node.range,
                    "Expression call has no matching definition authority");
                return false;
            }
            const auto definitionIndex = node.referenceIndex;
            const auto argumentCount = node.argumentCount;
            const auto arguments = node.arguments;
            const auto callRange = node.range;
            const auto& definition =
                definitions_->definitions[definitionIndex];
            if (argumentCount != definition.parameters.size()
                || nextLocalSlot_
                    > ExpressionLimits::maximumDefinitionLocals
                        - argumentCount)
            {
                failure = makeDiagnostic(
                    argumentCount != definition.parameters.size()
                        ? Diagnostic::Code::InvalidArgumentCount
                        : Diagnostic::Code::BudgetExceeded,
                    callRange,
                    "Expression call exceeds its fixed argument or local budget");
                return false;
            }

            std::vector<std::uint16_t> localSlots;
            localSlots.reserve(argumentCount);
            maximumDepth = 0;
            for (std::size_t index = 0; index < argumentCount; ++index)
            {
                const auto slot = static_cast<std::uint16_t>(nextLocalSlot_++);
                localSlots.push_back(slot);
                std::size_t argumentDepth = 0;
                if (!compileNode(arguments[index], argumentDepth, failure))
                    return false;
                maximumDepth = std::max(maximumDepth, argumentDepth);
                detail::Instruction store;
                store.op = detail::InstructionOp::StoreLocal;
                store.range = storage_.nodes[arguments[index]].range;
                store.referenceIndex = slot;
                store.domain = definition.parameters[index].domain;
                if (!append(store, failure))
                    return false;
            }

            const auto imported = importDefinitionBody(
                definition,
                localSlots,
                failure);
            if (!imported.has_value())
            {
                if (!failure.definitionIndex.has_value())
                    failure.definitionIndex = definitionIndex;
                return false;
            }
            std::size_t bodyDepth = 0;
            const auto previousDefinitionIndex = currentDefinitionIndex_;
            currentDefinitionIndex_ = definitionIndex;
            if (!compileNode(*imported, bodyDepth, failure))
            {
                currentDefinitionIndex_ = previousDefinitionIndex;
                if (!failure.definitionIndex.has_value())
                    failure.definitionIndex = definitionIndex;
                return false;
            }
            detail::Instruction validate;
            validate.op = detail::InstructionOp::ValidateDomain;
            validate.range = storage_.nodes[*imported].range;
            validate.domain = definition.resultDomain;
            if (!append(validate, failure))
            {
                currentDefinitionIndex_ = previousDefinitionIndex;
                return false;
            }
            currentDefinitionIndex_ = previousDefinitionIndex;
            maximumDepth = std::max(maximumDepth, bodyDepth);
            return true;
        }
        case detail::NodeKind::Call:
            break;
        }

        if (node.functionOp == detail::FunctionOp::If)
        {
            std::size_t conditionDepth = 0;
            if (!compileNode(node.arguments[0], conditionDepth, failure))
                return false;
            const auto conditionalIndex = storage_.instructions.size();
            if (!append(
                    {detail::InstructionOp::JumpIfFalse, node.range},
                    failure))
            {
                return false;
            }
            std::size_t trueDepth = 0;
            if (!compileNode(node.arguments[1], trueDepth, failure))
                return false;
            const auto jumpIndex = storage_.instructions.size();
            if (!append({detail::InstructionOp::Jump, node.range}, failure))
                return false;
            const auto falseTarget = storage_.instructions.size();
            std::size_t falseDepth = 0;
            if (!compileNode(node.arguments[2], falseDepth, failure))
                return false;
            const auto endTarget = storage_.instructions.size();
            if (falseTarget > std::numeric_limits<std::uint16_t>::max()
                || endTarget > std::numeric_limits<std::uint16_t>::max())
            {
                return false;
            }
            storage_.instructions[conditionalIndex].target =
                static_cast<std::uint16_t>(falseTarget);
            storage_.instructions[jumpIndex].target =
                static_cast<std::uint16_t>(endTarget);
            maximumDepth = std::max(
                conditionDepth,
                std::max(trueDepth, falseDepth));
            return true;
        }

        if (node.functionOp == detail::FunctionOp::EventVariableLoad
            || node.functionOp == detail::FunctionOp::EventVariableStore)
        {
            // A read gives its initial value until the variable is first
            // assigned; an assignment gives the value it assigns.
            const auto& variable = node.functionOp == detail::FunctionOp::EventVariableLoad
                ? node : storage_.nodes[node.arguments[0]];
            std::size_t argumentDepth = 0;
            if (!compileNode(node.arguments[1], argumentDepth, failure))
                return false;
            maximumDepth = std::max<std::size_t>(1, argumentDepth);
            detail::Instruction instruction;
            instruction.op = instructionFor(node.functionOp);
            instruction.range = node.range;
            instruction.referenceIndex = static_cast<std::uint16_t>(storage_.nodes[variable.arguments[0]].number);
            return append(instruction, failure);
        }
        if (node.functionOp == detail::FunctionOp::Sequence)
        {
            std::size_t leftDepth = 0;
            std::size_t rightDepth = 0;
            if (!compileNode(node.arguments[0], leftDepth, failure)
                || !compileNode(node.arguments[1], rightDepth, failure))
            {
                return false;
            }
            maximumDepth = std::max(leftDepth, 1 + rightDepth);
            detail::Instruction instruction;
            instruction.op = detail::InstructionOp::Sequence;
            instruction.range = node.range;
            instruction.number = leavesNothing(storage_, node.arguments[1]) ? 0.0 : 1.0;
            return append(instruction, failure);
        }
        if (node.functionOp == detail::FunctionOp::UpdateInternalState
            || node.functionOp == detail::FunctionOp::EveryMillisecond)
        {
            if (node.functionOp == detail::FunctionOp::EveryMillisecond)
                storage_.usesTime = true;
            return compileNode(node.arguments[0], maximumDepth, failure);
        }
        if (node.functionOp == detail::FunctionOp::Nothing)
        {
            maximumDepth = 1;
            return append({detail::InstructionOp::PushLiteral, node.range, 0.0}, failure);
        }
        if (node.functionOp == detail::FunctionOp::Shared)
        {
            // A temporary's expression read again shares the state cells it
            // had where it was first read, so its time
            // functions run as one.
            // Kept by index: the expression may hold shared ones of its own.
            const auto key = storage_.nodes[node.arguments[0]].number;
            const auto shared = static_cast<std::size_t>(
                std::ranges::find(sharedSlots_, key, &SharedSlots::key) - sharedSlots_.begin());
            const auto known = shared < sharedSlots_.size();
            const auto resume = nextStateSlot_;
            // Replaying an expression that first read this one here, it
            // takes its cells again in turn.
            const auto replayed = known && resume == sharedSlots_[shared].first;
            if (known)
                nextStateSlot_ = sharedSlots_[shared].first;
            if (!compileNode(node.arguments[1], maximumDepth, failure))
                return false;
            if (!known)
                sharedSlots_.push_back({key, resume, nextStateSlot_});
            else if (nextStateSlot_ != sharedSlots_[shared].end)
            {
                failure = makeDiagnostic(Diagnostic::Code::BudgetExceeded, node.range,
                    "A shared temporary should keep its state cells");
                return false;
            }
            else if (!replayed)
                nextStateSlot_ = resume;
            return true;
        }

        if (node.functionOp == detail::FunctionOp::Recurrence)
        {
            // Its value last time waits in a state cell; the argument reads
            // it, through a local, as talasRecurrentValue.
            if (!supportsStatefulExpressions(storage_.languageVersion)
                || nextStateSlot_ + 1 > ExpressionLimits::maximumStateSlots
                || nextLocalSlot_ + 1 > ExpressionLimits::maximumDefinitionLocals)
            {
                failure = makeDiagnostic(Diagnostic::Code::BudgetExceeded, node.range,
                    "Expression exceeds or does not support stateful storage");
                return false;
            }
            const auto stateSlot = static_cast<std::uint16_t>(nextStateSlot_++);
            const auto localSlot = static_cast<std::uint16_t>(nextLocalSlot_++);
            detail::Instruction load;
            load.op = detail::InstructionOp::RecurrenceLoad;
            load.range = node.range;
            load.referenceIndex = stateSlot;
            detail::Instruction store;
            store.op = detail::InstructionOp::StoreLocal;
            store.range = node.range;
            store.referenceIndex = localSlot;
            store.domain = ValueDomain::Scalar;
            if (!append(load, failure) || !append(store, failure))
                return false;
            recurrenceLocals_.push_back(localSlot);
            std::size_t argumentDepth = 0;
            const auto compiled = compileNode(node.arguments[0], argumentDepth, failure);
            recurrenceLocals_.pop_back();
            if (!compiled)
                return false;
            maximumDepth = std::max<std::size_t>(1, argumentDepth);
            detail::Instruction remember;
            remember.op = detail::InstructionOp::RecurrenceStore;
            remember.range = node.range;
            remember.referenceIndex = stateSlot;
            return append(remember, failure);
        }

        if (isStatefulFunction(node.functionOp))
        {
            // Smooth keeps its start and its target in two cells; the
            // other two-cell messages keep a second edge, and
            // durationBetweenTriggers: keeps the times of its last ten
            // triggers in five more.
            const auto slots = node.functionOp == detail::FunctionOp::TriggerDuration
                ? std::size_t{6}
                : node.functionOp == detail::FunctionOp::Smooth
                    || node.functionOp == detail::FunctionOp::TriggerEvery
                    || node.functionOp == detail::FunctionOp::ChangedReset
                    || node.functionOp == detail::FunctionOp::SustainBy
                    || node.functionOp == detail::FunctionOp::Chaotic
                    || node.functionOp == detail::FunctionOp::SwarmFollow
                    || node.functionOp == detail::FunctionOp::MidiTimingClock
                ? std::size_t{2}
                : std::size_t{1};
            if (!supportsStatefulExpressions(storage_.languageVersion)
                || nextStateSlot_ + slots
                    > ExpressionLimits::maximumStateSlots)
            {
                failure = makeDiagnostic(
                    supportsStatefulExpressions(storage_.languageVersion)
                        ? Diagnostic::Code::BudgetExceeded
                        : Diagnostic::Code::UnknownFunction,
                    node.range,
                    "Expression exceeds or does not support stateful storage");
                return false;
            }
            maximumDepth = 0;
            for (std::size_t index = 0; index < node.argumentCount; ++index)
            {
                std::size_t argumentDepth = 0;
                if (!compileNode(node.arguments[index], argumentDepth, failure))
                    return false;
                maximumDepth = std::max(maximumDepth, index + argumentDepth);
            }
            detail::Instruction instruction;
            instruction.op = instructionFor(node.functionOp);
            instruction.range = node.range;
            instruction.number = node.number;
            instruction.referenceIndex = static_cast<std::uint16_t>(
                nextStateSlot_);
            nextStateSlot_ += slots;
            // Messages that follow only their triggers and arguments are not
            // functions of time.
            if (node.functionOp != detail::FunctionOp::Previous
                && node.functionOp != detail::FunctionOp::Accumulate
                && node.functionOp != detail::FunctionOp::SampleHold
                && node.functionOp != detail::FunctionOp::NextRandom
                && node.functionOp != detail::FunctionOp::CountTriggers
                && node.functionOp != detail::FunctionOp::TriggerEvery
                && node.functionOp != detail::FunctionOp::Toggle
                && node.functionOp != detail::FunctionOp::GateWhen
                && node.functionOp != detail::FunctionOp::ChangedReset
                && node.functionOp != detail::FunctionOp::SustainBy
                && node.functionOp != detail::FunctionOp::NextRandomSeeded
                && node.functionOp != detail::FunctionOp::Chaotic
                && node.functionOp != detail::FunctionOp::TrackHold
                && node.functionOp != detail::FunctionOp::Initially)
            {
                storage_.usesTime = true;
            }
            if (node.functionOp == detail::FunctionOp::Initially)
                storage_.startsFromItsStartingValues = true;
            return append(instruction, failure);
        }

        maximumDepth = 0;
        for (std::size_t index = 0; index < node.argumentCount; ++index)
        {
            std::size_t argumentDepth = 0;
            if (!compileNode(node.arguments[index], argumentDepth, failure))
                return false;
            maximumDepth = std::max(maximumDepth, index + argumentDepth);
        }
        return append(
            {instructionFor(node.functionOp), node.range, node.number},
            failure);
    }

    detail::ExpressionStorage& storage_;
    const ExpressionDefinitionLibrary* definitions_ = nullptr;
    std::size_t nextLocalSlot_ = 0;
    std::size_t nextStateSlot_ = 0;
    struct SharedSlots
    {
        double key = 0.0;
        std::size_t first = 0;
        std::size_t end = 0;
    };
    std::vector<SharedSlots> sharedSlots_;
    std::vector<std::uint16_t> recurrenceLocals_;
    std::optional<std::size_t> currentDefinitionIndex_;
};

[[nodiscard]] bool validateDefinitionPrograms(
    ExpressionLanguageVersion languageVersion,
    const ExpressionDefinitionLibrary& library,
    Diagnostic& failure)
{
    if (!supportsExpressionDefinitions(languageVersion))
        return true;

    for (std::size_t definitionIndex = 0;
         definitionIndex < library.definitions.size();
         ++definitionIndex)
    {
        const auto& definition = library.definitions[definitionIndex];
        std::string invocation = definition.name;
        invocation += '(';
        for (std::size_t parameterIndex = 0;
             parameterIndex < definition.parameters.size();
             ++parameterIndex)
        {
            if (parameterIndex != 0)
                invocation += ',';
            invocation += '0';
        }
        invocation += ')';

        auto parsed = Parser(
            invocation,
            languageVersion,
            &library).parse();
        if (!parsed.storage.has_value())
        {
            failure = parsed.diagnostics.empty()
                ? makeDiagnostic(
                    Diagnostic::Code::InvalidDefinition,
                    {},
                    "Expression definition cannot be called")
                : std::move(parsed.diagnostics.front());
            if (!failure.definitionIndex.has_value())
                failure.definitionIndex = definitionIndex;
            return false;
        }

        if (!Compiler(*parsed.storage, &library).compile(failure))
        {
            if (!failure.definitionIndex.has_value())
                failure.definitionIndex = definitionIndex;
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool isPositiveFinite(double value) noexcept
{
    return std::isfinite(value) && value > 0.0;
}

[[nodiscard]] bool isTimeDomain(ValueDomain domain) noexcept
{
    return domain == ValueDomain::Seconds
        || domain == ValueDomain::Samples;
}

class LegacyEvaluator
{
public:
    LegacyEvaluator(
        const detail::ExpressionStorage& storage,
        const EvaluationContext& context,
        bool collectDiagnostics)
        : storage_(storage),
          context_(context),
          collectDiagnostics_(collectDiagnostics)
    {
    }

    [[nodiscard]] EvaluationResult evaluate(EvaluationOptions options)
    {
        EvaluationResult result;
        const auto value = evaluateValue(options);
        result.diagnostics = std::move(diagnostics_);
        if (value.has_value() && result.diagnostics.empty())
            result.value = *value;
        return result;
    }

    [[nodiscard]] bool tryEvaluate(
        EvaluationOptions options,
        ExpressionValue& result) noexcept
    {
        const auto value = evaluateValue(options);
        if (!value.has_value())
            return false;
        result = *value;
        return true;
    }

private:
    [[nodiscard]] std::optional<ExpressionValue> evaluateValue(
        EvaluationOptions options)
    {
        auto value = evaluateNode(storage_.root);
        if (value.has_value())
        {
            value = convertToExpected(
                *value,
                options.expectedDomain,
                storage_.nodes[storage_.root].range);
        }
        return value;
    }

    [[nodiscard]] std::optional<ExpressionValue> evaluateNode(NodeId id)
    {
        if (id >= storage_.nodes.size())
            return std::nullopt;
        const auto& node = storage_.nodes[id];
        switch (node.kind)
        {
        case detail::NodeKind::Literal:
            return evaluateLiteral(node);
        case detail::NodeKind::EventReference:
            return resolveReference(node, ReferenceKind::Event);
        case detail::NodeKind::VariableReference:
            return resolveReference(node, ReferenceKind::Variable);
        case detail::NodeKind::IdentifierReference:
            return resolveReference(node, ReferenceKind::Identifier);
        case detail::NodeKind::Unary:
            return evaluateUnary(node);
        case detail::NodeKind::Binary:
            return evaluateBinary(node);
        case detail::NodeKind::Comparison:
        case detail::NodeKind::UnitApplication:
        case detail::NodeKind::Call:
        case detail::NodeKind::ListIndex:
        case detail::NodeKind::ArrayMap:
        case detail::NodeKind::SeriesIndex:
        case detail::NodeKind::FormalReference:
        case detail::NodeKind::UserCall:
            return std::nullopt;
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<ExpressionValue> evaluateLiteral(
        const detail::ExpressionNode& node)
    {
        auto value = ExpressionValue{};
        switch (node.unit)
        {
        case Unit::None:
            value = {node.number, ValueDomain::Scalar};
            break;
        case Unit::Hertz:
            value = {node.number, ValueDomain::Hertz};
            break;
        case Unit::Decibels:
            value = {
                std::pow(10.0, node.number / 20.0),
                ValueDomain::Scalar};
            break;
        case Unit::Milliseconds:
            value = {node.number / 1000.0, ValueDomain::Seconds};
            break;
        case Unit::Microseconds:
            value = {node.number / 1000000.0, ValueDomain::Seconds};
            break;
        case Unit::Seconds:
            value = {node.number, ValueDomain::Seconds};
            break;
        case Unit::Minutes:
            value = {node.number * 60.0, ValueDomain::Seconds};
            break;
        case Unit::Hours:
            value = {node.number * 3600.0, ValueDomain::Seconds};
            break;
        case Unit::Days:
            value = {node.number * 86400.0, ValueDomain::Seconds};
            break;
        case Unit::NoteNumber:
            value = {node.number, ValueDomain::NoteNumber};
            break;
        case Unit::Beats:
            if (!isPositiveFinite(context_.tempoBpm))
            {
                addDiagnostic(
                    Diagnostic::Code::MissingTempo,
                    node.range,
                    "A positive finite tempo is required to evaluate beats");
                return std::nullopt;
            }
            value = {
                node.number * 60.0 / context_.tempoBpm,
                ValueDomain::Seconds};
            break;
        case Unit::Samples:
            value = {node.number, ValueDomain::Samples};
            break;
        case Unit::BeatsPerMinute:
            value = {node.number / 60.0, ValueDomain::Hertz};
            break;
        }
        return checkedResult(value, node.range);
    }

    [[nodiscard]] std::optional<ExpressionValue> resolveReference(
        const detail::ExpressionNode& node,
        ReferenceKind kind)
    {
        ExpressionValue value;
        const auto resolver = kind == ReferenceKind::Event
            ? context_.resolveEvent
            : kind == ReferenceKind::Variable
                ? context_.resolveVariable
                : context_.resolveIdentifier;
        const auto code = kind == ReferenceKind::Event
            ? Diagnostic::Code::UnknownEvent
            : kind == ReferenceKind::Variable
                ? Diagnostic::Code::UnknownVariable
                : Diagnostic::Code::UnknownIdentifier;
        if (resolver == nullptr
            || !resolver(context_.user, node.name, value))
        {
            if (collectDiagnostics_)
            {
                addDiagnostic(
                    code,
                    node.range,
                    std::string{kind == ReferenceKind::Event
                            ? "Unknown event reference: "
                            : kind == ReferenceKind::Variable
                                ? "Unknown variable reference: "
                                : "Unknown identifier: "}
                        + node.name);
            }
            return std::nullopt;
        }
        if (!std::isfinite(value.value))
        {
            addDiagnostic(
                Diagnostic::Code::NonFiniteResult,
                node.range,
                "Reference resolved to a non-finite value");
            return std::nullopt;
        }
        return value;
    }

    [[nodiscard]] std::optional<ExpressionValue> evaluateUnary(
        const detail::ExpressionNode& node)
    {
        auto value = evaluateNode(node.left);
        if (!value.has_value())
            return std::nullopt;
        if (node.unaryOp == UnaryOp::Negate)
            value->value = -value->value;
        return checkedResult(*value, node.range);
    }

    [[nodiscard]] std::optional<ExpressionValue> evaluateBinary(
        const detail::ExpressionNode& node)
    {
        const auto left = evaluateNode(node.left);
        const auto right = evaluateNode(node.right);
        if (!left.has_value() || !right.has_value())
            return std::nullopt;
        switch (node.binaryOp)
        {
        case BinaryOp::Add:
            return evaluateAddSubtract(*left, *right, node.range, false);
        case BinaryOp::Subtract:
            return evaluateAddSubtract(*left, *right, node.range, true);
        case BinaryOp::Multiply:
            return evaluateMultiply(*left, *right, node.range);
        case BinaryOp::Divide:
            return evaluateDivide(*left, *right, node.range);
        case BinaryOp::Remainder:
            return evaluateRemainder(*left, *right, node.range);
        case BinaryOp::Power:
        case BinaryOp::IntegerDivide:
        case BinaryOp::Modulo:
            // Val operators never reach the legacy evaluator.
            return std::nullopt;
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<ExpressionValue> evaluateAddSubtract(
        ExpressionValue left,
        ExpressionValue right,
        SourceRange range,
        bool subtract)
    {
        if (left.domain != right.domain
            && isTimeDomain(left.domain)
            && isTimeDomain(right.domain))
        {
            const auto leftSeconds = normalizeTimeValue(left, range);
            const auto rightSeconds = normalizeTimeValue(right, range);
            if (!leftSeconds.has_value() || !rightSeconds.has_value())
                return std::nullopt;
            return checkedResult(
                {subtract
                     ? leftSeconds->value - rightSeconds->value
                     : leftSeconds->value + rightSeconds->value,
                 ValueDomain::Seconds},
                range);
        }
        if (left.domain != right.domain)
        {
            addDiagnostic(
                Diagnostic::Code::UnitMismatch,
                range,
                "Addition and subtraction require matching value domains");
            return std::nullopt;
        }
        return checkedResult(
            {subtract ? left.value - right.value : left.value + right.value,
             left.domain},
            range);
    }

    [[nodiscard]] std::optional<ExpressionValue> evaluateMultiply(
        ExpressionValue left,
        ExpressionValue right,
        SourceRange range)
    {
        ExpressionValue result;
        if (left.domain == ValueDomain::Scalar
            && right.domain == ValueDomain::Scalar)
        {
            result = {left.value * right.value, ValueDomain::Scalar};
        }
        else if (left.domain == ValueDomain::Scalar)
        {
            result = {left.value * right.value, right.domain};
        }
        else if (right.domain == ValueDomain::Scalar)
        {
            result = {left.value * right.value, left.domain};
        }
        else
        {
            addDiagnostic(
                Diagnostic::Code::UnitMismatch,
                range,
                "Multiplication requires at least one scalar operand");
            return std::nullopt;
        }
        return checkedResult(result, range);
    }

    [[nodiscard]] std::optional<ExpressionValue> evaluateDivide(
        ExpressionValue left,
        ExpressionValue right,
        SourceRange range)
    {
        if (right.value == 0.0)
        {
            addDiagnostic(
                Diagnostic::Code::DivisionByZero,
                range,
                "Division by zero");
            return std::nullopt;
        }
        ExpressionValue result;
        if (left.domain == ValueDomain::Scalar
            && right.domain == ValueDomain::Scalar)
        {
            result = {left.value / right.value, ValueDomain::Scalar};
        }
        else if (right.domain == ValueDomain::Scalar)
        {
            result = {left.value / right.value, left.domain};
        }
        else if (left.domain == ValueDomain::Scalar
                 && right.domain == ValueDomain::Seconds)
        {
            result = {left.value / right.value, ValueDomain::Hertz};
        }
        else if (left.domain == ValueDomain::Scalar
                 && right.domain == ValueDomain::Hertz)
        {
            result = {left.value / right.value, ValueDomain::Seconds};
        }
        // A frequency, time or pitch divided by another of its kind is
        // their ratio, as 4 c sharp hz / 4 c hz is and !KeyNumber nn /
        // 127 nn is.
        else if (left.domain == right.domain)
        {
            result = {left.value / right.value, ValueDomain::Scalar};
        }
        else
        {
            addDiagnostic(
                Diagnostic::Code::UnitMismatch,
                range,
                "Division operands have incompatible value domains");
            return std::nullopt;
        }
        return checkedResult(result, range);
    }

    [[nodiscard]] std::optional<ExpressionValue> evaluateRemainder(
        ExpressionValue left,
        ExpressionValue right,
        SourceRange range)
    {
        if (left.domain != ValueDomain::Scalar
            || right.domain != ValueDomain::Scalar)
        {
            addDiagnostic(
                Diagnostic::Code::UnitMismatch,
                range,
                "Remainder requires scalar operands");
            return std::nullopt;
        }
        if (right.value == 0.0)
        {
            addDiagnostic(
                Diagnostic::Code::DivisionByZero,
                range,
                "Remainder by zero");
            return std::nullopt;
        }
        auto value = std::fmod(left.value, right.value);
        if (value == 0.0)
            value = 0.0;
        return checkedResult(
            {value, ValueDomain::Scalar},
            range);
    }

    [[nodiscard]] std::optional<ExpressionValue> convertToExpected(
        ExpressionValue value,
        ValueDomain expectedDomain,
        SourceRange range)
    {
        if (value.domain == expectedDomain)
            return checkedResult(value, range);
        if (value.domain == ValueDomain::NoteNumber
            && expectedDomain == ValueDomain::Hertz)
        {
            value.value = 440.0
                * std::pow(2.0, (value.value - 69.0) / 12.0);
            value.domain = ValueDomain::Hertz;
            return checkedResult(value, range);
        }
        if (value.domain == ValueDomain::Samples
            && expectedDomain == ValueDomain::Seconds)
        {
            if (!isPositiveFinite(context_.sampleRate))
            {
                addDiagnostic(
                    Diagnostic::Code::MissingSampleRate,
                    range,
                    "A positive finite sample rate is required to evaluate samples as seconds");
                return std::nullopt;
            }
            value.value /= context_.sampleRate;
            value.domain = ValueDomain::Seconds;
            return checkedResult(value, range);
        }
        addDiagnostic(
            Diagnostic::Code::UnitMismatch,
            range,
            "Expression value domain does not match the expected domain");
        return std::nullopt;
    }

    [[nodiscard]] std::optional<ExpressionValue> normalizeTimeValue(
        ExpressionValue value,
        SourceRange range)
    {
        if (value.domain == ValueDomain::Seconds)
            return checkedResult(value, range);
        if (value.domain == ValueDomain::Samples)
        {
            if (!isPositiveFinite(context_.sampleRate))
            {
                addDiagnostic(
                    Diagnostic::Code::MissingSampleRate,
                    range,
                    "A positive finite sample rate is required to evaluate samples as seconds");
                return std::nullopt;
            }
            value.value /= context_.sampleRate;
            value.domain = ValueDomain::Seconds;
            return checkedResult(value, range);
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<ExpressionValue> checkedResult(
        ExpressionValue value,
        SourceRange range)
    {
        if (!std::isfinite(value.value))
        {
            addDiagnostic(
                Diagnostic::Code::NonFiniteResult,
                range,
                "Expression produced a non-finite value");
            return std::nullopt;
        }
        return value;
    }

    void addDiagnostic(
        Diagnostic::Code code,
        SourceRange range,
        std::string_view message)
    {
        if (collectDiagnostics_)
        {
            diagnostics_.push_back(
                makeDiagnostic(code, range, std::string{message}));
        }
    }

    const detail::ExpressionStorage& storage_;
    const EvaluationContext& context_;
    bool collectDiagnostics_ = true;
    std::vector<Diagnostic> diagnostics_;
};

struct ExecutionFailure
{
    Diagnostic::Code code = Diagnostic::Code::UnexpectedToken;
    SourceRange range;
    std::size_t instructionIndex = 0;
};

struct ExecutionResult
{
    bool ok = false;
    ExpressionValue value;
    ExecutionFailure failure;
};

class Executor
{
public:
    Executor(
        const detail::ExpressionStorage& storage,
        const EvaluationContext& context,
        const ExpressionState* state = nullptr,
        ExpressionState* candidateState = nullptr) noexcept
        : storage_(storage), context_(context), state_(state),
          pausesSkippedTime_(storage.languageVersion == ExpressionLanguageVersion::ValV1
              && !storage.timeRunsOnInSkippedBranches && storage.stateSlotCount > 0)
    {
        if (state_ != nullptr)
        {
            candidateState_ = candidateState == nullptr
                ? &ownedCandidateState_
                : candidateState;
            *candidateState_ = *state_;
        }
    }

    [[nodiscard]] ExecutionResult execute(EvaluationOptions options) noexcept
    {
        if (storage_.voiceNumberInstruction.has_value())
        {
            instructionIndex_ = *storage_.voiceNumberInstruction;
            (void) fail(Diagnostic::Code::UnknownIdentifier,
                storage_.instructions[instructionIndex_].range);
            return {false, {}, failure_};
        }
        if (storage_.stateSlotCount > 0
            && (state_ == nullptr
                || state_->slotCount != storage_.stateSlotCount
                || state_->slotCount > state_->cells.size()))
        {
            (void) fail(
                Diagnostic::Code::MissingState,
                storage_.nodes[storage_.root].range);
            return {false, {}, failure_};
        }
        for (instructionIndex_ = 0;
             instructionIndex_ < storage_.instructions.size();)
        {
            const auto& instruction =
                storage_.instructions[instructionIndex_];
            if (!executeInstruction(instruction))
                return {false, {}, failure_};
        }
        if (stackSize_ != 1)
        {
            (void) fail(
                Diagnostic::Code::BudgetExceeded,
                storage_.nodes[storage_.root].range);
            return {false, {}, failure_};
        }
        auto value = stack_[0];
        if (!convertToExpected(
                value,
                options.expectedDomain,
                storage_.nodes[storage_.root].range))
        {
            return {false, {}, failure_};
        }
        if (pausesSkippedTime_ && candidateState_ != nullptr
            && std::isfinite(context_.sampleRate) && context_.sampleRate > 0.0)
        {
            const auto tickValue = std::floor(
                static_cast<double>(context_.elapsedSampleFrame) * 1000.0 / context_.sampleRate);
            if (std::isfinite(tickValue) && tickValue >= 0.0 && tickValue < 9007199254740992.0)
                candidateState_->lastEvaluationTick = static_cast<std::uint64_t>(tickValue) + 1;
        }
        return {true, value, {}};
    }

    void commitState(ExpressionState& state) const noexcept
    {
        if (candidateState_ != nullptr)
            state = *candidateState_;
    }

private:
    [[nodiscard]] bool push(ExpressionValue value) noexcept
    {
        if (stackSize_ >= storage_.maximumStackDepth
            || stackSize_ >= stack_.size())
        {
            return fail(
                Diagnostic::Code::BudgetExceeded,
                currentRange());
        }
        stack_[stackSize_++] = value;
        return true;
    }

    [[nodiscard]] bool pop(ExpressionValue& value) noexcept
    {
        if (stackSize_ == 0)
        {
            return fail(
                Diagnostic::Code::BudgetExceeded,
                currentRange());
        }
        value = stack_[--stackSize_];
        return true;
    }

    [[nodiscard]] SourceRange currentRange() const noexcept
    {
        return instructionIndex_ < storage_.instructions.size()
            ? storage_.instructions[instructionIndex_].range
            : storage_.nodes[storage_.root].range;
    }

    [[nodiscard]] bool fail(
        Diagnostic::Code code,
        SourceRange range) noexcept
    {
        failure_ = {code, range, instructionIndex_};
        return false;
    }

    // Under the project's choice the inverse of 0
    // is infinity, a value arithmetic carries on; NaN stays undefined.
    [[nodiscard]] bool admissible(double value) const noexcept
    {
        return std::isfinite(value) || (context_.inverseOfZeroIsInfinite && std::isinf(value));
    }

    [[nodiscard]] bool checked(
        ExpressionValue value,
        SourceRange range) noexcept
    {
        if (!admissible(value.value))
            return fail(Diagnostic::Code::NonFiniteResult, range);
        return push(value);
    }

    [[nodiscard]] bool normalizeTime(
        ExpressionValue& value,
        SourceRange range) noexcept
    {
        if (value.domain == ValueDomain::Seconds)
            return true;
        if (value.domain != ValueDomain::Samples)
            return false;
        if (!isPositiveFinite(context_.sampleRate))
            return fail(Diagnostic::Code::MissingSampleRate, range);
        value.value /= context_.sampleRate;
        value.domain = ValueDomain::Seconds;
        if (!std::isfinite(value.value))
            return fail(Diagnostic::Code::NonFiniteResult, range);
        return true;
    }

    [[nodiscard]] bool makeDomainsCompatible(
        ExpressionValue& left,
        ExpressionValue& right,
        SourceRange range) noexcept
    {
        if (left.domain == right.domain)
            return true;
        if (isTimeDomain(left.domain) && isTimeDomain(right.domain))
            return normalizeTime(left, range) && normalizeTime(right, range);
        // Val adds a note number and a frequency in hertz (60 nn + 100 hz
        // is 361.6256 hz).
        if (usesValMessages(storage_.languageVersion)
            && ((left.domain == ValueDomain::NoteNumber && right.domain == ValueDomain::Hertz)
                || (left.domain == ValueDomain::Hertz && right.domain == ValueDomain::NoteNumber)))
        {
            return convertVal(left, ValueDomain::Hertz, range)
                && convertVal(right, ValueDomain::Hertz, range);
        }
        // A plain number beside a value with units is in its units, as
        // 2.39415 s + ((1 - !Rate) * 10) is in seconds.
        if (usesValMessages(storage_.languageVersion)
            && (left.domain == ValueDomain::Scalar) != (right.domain == ValueDomain::Scalar))
        {
            (left.domain == ValueDomain::Scalar ? left : right).domain =
                left.domain == ValueDomain::Scalar ? right.domain : left.domain;
            return true;
        }
        return fail(Diagnostic::Code::UnitMismatch, range);
    }

    [[nodiscard]] bool makeThreeDomainsCompatible(
        ExpressionValue& first,
        ExpressionValue& second,
        ExpressionValue& third,
        SourceRange range) noexcept
    {
        if (first.domain == second.domain
            && first.domain == third.domain)
        {
            return true;
        }
        if (isTimeDomain(first.domain)
            && isTimeDomain(second.domain)
            && isTimeDomain(third.domain))
        {
            return normalizeTime(first, range)
                && normalizeTime(second, range)
                && normalizeTime(third, range);
        }
        return fail(Diagnostic::Code::UnitMismatch, range);
    }

    [[nodiscard]] bool executeInstruction(
        const detail::Instruction& instruction) noexcept
    {
        switch (instruction.op)
        {
        case detail::InstructionOp::PushLiteral:
            return executeLiteral(instruction);
        case detail::InstructionOp::ApplyUnit:
            return executeUnitApplication(instruction);
        case detail::InstructionOp::ResolveEvent:
            return executeReference(instruction, ReferenceKind::Event);
        case detail::InstructionOp::ResolveVariable:
            return executeReference(instruction, ReferenceKind::Variable);
        case detail::InstructionOp::ResolveIdentifier:
            return executeReference(instruction, ReferenceKind::Identifier);
        case detail::InstructionOp::Negate:
            return executeNegate(instruction);
        case detail::InstructionOp::Add:
        case detail::InstructionOp::Subtract:
        case detail::InstructionOp::Multiply:
        case detail::InstructionOp::Divide:
        case detail::InstructionOp::Remainder:
        case detail::InstructionOp::Power:
        case detail::InstructionOp::IntegerDivide:
        case detail::InstructionOp::Modulo:
            return executeBinary(instruction);
        case detail::InstructionOp::Less:
        case detail::InstructionOp::LessEqual:
        case detail::InstructionOp::Greater:
        case detail::InstructionOp::GreaterEqual:
        case detail::InstructionOp::Equal:
        case detail::InstructionOp::NotEqual:
            return executeComparison(instruction);
        case detail::InstructionOp::Abs:
        case detail::InstructionOp::Sqrt:
        case detail::InstructionOp::Log:
        case detail::InstructionOp::Exp:
        case detail::InstructionOp::Log10:
        case detail::InstructionOp::Log2:
        case detail::InstructionOp::Exp2:
        case detail::InstructionOp::Squared:
        case detail::InstructionOp::Reciprocal:
        case detail::InstructionOp::Sign:
        case detail::InstructionOp::Truncate:
        case detail::InstructionOp::Round:
        case detail::InstructionOp::ClipTo01:
        case detail::InstructionOp::WrapTo02:
        case detail::InstructionOp::Cos:
        case detail::InstructionOp::Sin:
        case detail::InstructionOp::NormCos:
        case detail::InstructionOp::NormSin:
        case detail::InstructionOp::LogicValue:
        case detail::InstructionOp::RemoveUnits:
        case detail::InstructionOp::ArrayIndex:
        case detail::InstructionOp::Floor:
        case detail::InstructionOp::Ceiling:
        case detail::InstructionOp::RoundHalfUp:
        case detail::InstructionOp::LinearUnitlessSOSPitch:
        case detail::InstructionOp::ArcTan:
        case detail::InstructionOp::ArcSin:
        case detail::InstructionOp::ArcCos:
        case detail::InstructionOp::Tan:
        case detail::InstructionOp::Sinc:
            return executeUnaryFunction(instruction);
        case detail::InstructionOp::Min:
        case detail::InstructionOp::Max:
        case detail::InstructionOp::RoundTo:
        case detail::InstructionOp::TruncateTo:
        case detail::InstructionOp::CeilingTo:
        case detail::InstructionOp::MaxAbs:
        case detail::InstructionOp::MinAbs:
        case detail::InstructionOp::LogBase:
        case detail::InstructionOp::FloorLog:
            return executeBinaryFunction(instruction);
        case detail::InstructionOp::Between:
            return executeBetween(instruction);
        case detail::InstructionOp::Clamp:
            return executeClamp(instruction);
        case detail::InstructionOp::Map:
            return executeMap(instruction);
        case detail::InstructionOp::ArrayMap:
            return executeArrayMap(instruction);
        case detail::InstructionOp::SelectList:
            return executeListSelect(instruction);
        case detail::InstructionOp::ValidateSequenceIndex:
            return executeSequenceIndexValidation(instruction);
        case detail::InstructionOp::ValidateSequenceCount:
            return executeSequenceCountValidation(instruction);
        case detail::InstructionOp::CheckSequenceBounds:
            return executeSequenceBoundsCheck(instruction);
        case detail::InstructionOp::EvaluateSeries:
            return executeSeries(instruction);
        case detail::InstructionOp::StoreLocal:
            return executeStoreLocal(instruction);
        case detail::InstructionOp::LoadLocal:
            return executeLoadLocal(instruction);
        case detail::InstructionOp::PickLocal:
            return executePickLocal(instruction);
        case detail::InstructionOp::ValidateDomain:
            return executeValidateDomain(instruction);
        case detail::InstructionOp::JumpIfFalse:
            return executeJumpIfFalse(instruction);
        case detail::InstructionOp::Jump:
            return executeJump(instruction);
        case detail::InstructionOp::Previous:
            return executePrevious(instruction);
        case detail::InstructionOp::Accumulate:
            return executeAccumulate(instruction);
        case detail::InstructionOp::SampleHold:
            return executeSampleHold(instruction);
        case detail::InstructionOp::Ramp:
            return executeRamp(instruction);
        case detail::InstructionOp::TimeFunction:
            return executeTimeFunction(instruction);
        case detail::InstructionOp::Metronome:
            return executeMetronome(instruction);
        case detail::InstructionOp::MidiTimingClock:
            return executeMidiTimingClock(instruction);
        case detail::InstructionOp::RandomInterval:
        case detail::InstructionOp::NextRandom:
        case detail::InstructionOp::NextRandomSeeded:
            return executeRandom(instruction);
        case detail::InstructionOp::Smooth:
            return executeSmooth(instruction);
        case detail::InstructionOp::CountTriggers:
            return executeCountTriggers(instruction);
        case detail::InstructionOp::TriggerEvery:
            return executeTriggerEvery(instruction);
        case detail::InstructionOp::Toggle:
            return executeToggle(instruction);
        case detail::InstructionOp::GateWhen:
            return executeGateWhen(instruction);
        case detail::InstructionOp::TriggerDuration:
            return executeTriggerDuration(instruction);
        case detail::InstructionOp::ChangedReset:
            return executeChangedReset(instruction);
        case detail::InstructionOp::SustainBy:
            return executeSustainBy(instruction);
        case detail::InstructionOp::Chaotic:
            return executeChaotic(instruction);
        case detail::InstructionOp::SwarmFollow:
            return executeSwarmFollow(instruction);
        case detail::InstructionOp::TrackHold:
            return executeTrackHold(instruction);
        case detail::InstructionOp::Initially:
            return executeInitially(instruction);
        case detail::InstructionOp::RecurrenceLoad:
        case detail::InstructionOp::RecurrenceStore:
            return executeRecurrence(instruction);
        case detail::InstructionOp::EventVariableLoad:
        case detail::InstructionOp::EventVariableStore:
            return executeEventVariable(instruction);
        case detail::InstructionOp::Sequence:
            return executeSequence(instruction);
        case detail::InstructionOp::GateBegin:
        case detail::InstructionOp::GateEnd:
            return executeGate(instruction);
        }
        return false;
    }

    // An EventVariable gives its initial value until it is
    // first assigned; <~ and <+ assign it.
    [[nodiscard]] bool executeEventVariable(const detail::Instruction& instruction) noexcept
    {
        ExpressionValue value;
        if (!pop(value))
            return false;
        auto* cell = stateCell(instruction);
        if (cell == nullptr)
            return false;
        if (instruction.op == detail::InstructionOp::EventVariableStore)
        {
            cell->value = value;
            cell->initialized = true;
        }
        ++instructionIndex_;
        return checked(cell->initialized ? cell->value : value, instruction.range);
    }

    // Expressions in sequence leave the last value one of them
    // leaves.
    [[nodiscard]] bool executeSequence(const detail::Instruction& instruction) noexcept
    {
        ExpressionValue right;
        ExpressionValue left;
        if (!pop(right) || !pop(left))
            return false;
        ++instructionIndex_;
        return push(instruction.number != 0.0 ? right : left);
    }

    // A program with EventVariables that is a function of time
    // runs once a millisecond, giving the value it computed in between.
    [[nodiscard]] bool executeGate(const detail::Instruction& instruction) noexcept
    {
        if (!storage_.usesTime)
        {
            ++instructionIndex_;
            return true;
        }
        auto* cell = stateCell(instruction);
        if (cell == nullptr)
            return false;
        std::uint64_t tick = 0;
        if (!currentTick(instruction, *cell, tick))
            return false;
        if (instruction.op == detail::InstructionOp::GateBegin)
        {
            if (cell->initialized && cell->lastTimeTick == tick)
            {
                instructionIndex_ = instruction.target;
                return push(cell->value);
            }
            ++instructionIndex_;
            return true;
        }
        if (stackSize_ == 0)
            return fail(Diagnostic::Code::BudgetExceeded, instruction.range);
        cell->value = stack_[stackSize_ - 1];
        cell->initialized = true;
        cell->lastTimeTick = tick;
        ++instructionIndex_;
        return true;
    }

    // talasRecurrence: its value last time, 0 before it has one, and then
    // the value it gives now kept for next time.
    [[nodiscard]] bool executeRecurrence(const detail::Instruction& instruction) noexcept
    {
        auto* cell = stateCell(instruction);
        if (cell == nullptr)
            return false;
        if (instruction.op == detail::InstructionOp::RecurrenceLoad)
        {
            ++instructionIndex_;
            return checked(cell->initialized ? cell->value : ExpressionValue{0.0, ValueDomain::Scalar}, instruction.range);
        }
        ExpressionValue value;
        if (!pop(value))
            return false;
        cell->value = value;
        cell->initialized = true;
        ++instructionIndex_;
        return checked(value, instruction.range);
    }

    // The receiver of a trigger message and whether it rose since the last
    // evaluation: positive is on.
    [[nodiscard]] bool popTrigger(
        const detail::Instruction& instruction,
        ExpressionValue& trigger) noexcept
    {
        if (!pop(trigger))
            return false;
        if (trigger.domain != ValueDomain::Scalar)
            return fail(Diagnostic::Code::UnitMismatch, instruction.range);
        return true;
    }

    // A count argument: a finite number truncated, at least minimum.
    [[nodiscard]] bool countArgument(
        const detail::Instruction& instruction,
        const ExpressionValue& value,
        double minimum,
        double maximum,
        std::uint64_t& count) noexcept
    {
        if (value.domain != ValueDomain::Scalar || !std::isfinite(value.value)
            || std::trunc(value.value) < minimum || std::trunc(value.value) > maximum)
        {
            return fail(Diagnostic::Code::InvalidArgument, instruction.range);
        }
        count = static_cast<std::uint64_t>(std::trunc(value.value));
        return true;
    }

    // trigger countTriggers counts its triggers from 0, -1 before the first;
    // countTriggersMod: wraps at the count, and countTriggersMod:reverse:
    // counts down while its second argument is positive.
    [[nodiscard]] bool executeCountTriggers(
        const detail::Instruction& instruction) noexcept
    {
        ExpressionValue reverse;
        ExpressionValue modulo;
        ExpressionValue trigger;
        if (!pop(reverse) || !pop(modulo) || !popTrigger(instruction, trigger))
            return false;
        const auto wraps = instruction.number > 0.0;
        std::uint64_t count = 0;
        if (wraps && !countArgument(instruction, modulo, 1.0, 1.0e15, count))
            return false;
        if (reverse.domain != ValueDomain::Scalar || !std::isfinite(reverse.value))
            return fail(Diagnostic::Code::InvalidArgument, instruction.range);
        auto* cell = stateCell(instruction);
        if (cell == nullptr)
            return false;
        const auto high = trigger.value > 0.0;
        auto position = cell->initialized ? cell->value.value : -1.0;
        if (high && !cell->auxiliary)
        {
            if (!wraps)
            {
                position = position < 0.0 ? 0.0 : position + 1.0;
            }
            else
            {
                const auto modulus = static_cast<double>(count);
                const auto down = instruction.number > 1.0 && reverse.value > 0.0;
                if (position < 0.0)
                    position = down ? modulus - 1.0 : 0.0;
                else
                    position = std::fmod(std::fmod(position, modulus)
                        + (down ? modulus - 1.0 : 1.0), modulus);
            }
        }
        else if (wraps && position >= 0.0)
        {
            position = std::fmod(position, static_cast<double>(count));
        }
        cell->value = {position, ValueDomain::Scalar};
        cell->initialized = true;
        cell->auxiliary = high;
        ++instructionIndex_;
        return checked(cell->value, instruction.range);
    }

    // trigger triggerEvery: n passes the first of every n triggers.
    [[nodiscard]] bool executeTriggerEvery(
        const detail::Instruction& instruction) noexcept
    {
        ExpressionValue every;
        ExpressionValue trigger;
        if (!pop(every) || !popTrigger(instruction, trigger))
            return false;
        std::uint64_t count = 0;
        if (!countArgument(instruction, every, 1.0, 1.0e15, count))
            return false;
        auto* cell = stateCell(instruction);
        if (cell == nullptr)
            return false;
        if (instruction.referenceIndex + 1u >= candidateState_->slotCount)
            return fail(Diagnostic::Code::MissingState, instruction.range);
        auto* selected = cell + 1;
        const auto high = trigger.value > 0.0;
        if (high && !cell->auxiliary)
        {
            selected->auxiliary = std::fmod(cell->value.value, static_cast<double>(count)) == 0.0;
            cell->value.value += 1.0;
        }
        cell->initialized = true;
        cell->auxiliary = high;
        ++instructionIndex_;
        return push({high && selected->auxiliary ? 1.0 : 0.0, ValueDomain::Scalar});
    }

    // trigger asToggle turns between 0 and 1 on each trigger; toggleWhen:
    // only while its argument is positive.
    [[nodiscard]] bool executeToggle(
        const detail::Instruction& instruction) noexcept
    {
        ExpressionValue condition;
        ExpressionValue trigger;
        if (!pop(condition) || !popTrigger(instruction, trigger))
            return false;
        if (condition.domain != ValueDomain::Scalar || !std::isfinite(condition.value))
            return fail(Diagnostic::Code::InvalidArgument, instruction.range);
        auto* cell = stateCell(instruction);
        if (cell == nullptr)
            return false;
        const auto high = trigger.value > 0.0;
        if (!cell->initialized)
            cell->value = {0.0, ValueDomain::Scalar};
        if (high && !cell->auxiliary && condition.value > 0.0)
            cell->value.value = 1.0 - cell->value.value;
        cell->initialized = true;
        cell->auxiliary = high;
        ++instructionIndex_;
        return push(cell->value);
    }

    // trigger gateWhen: condition passes the gate begun while its argument
    // was positive.
    [[nodiscard]] bool executeGateWhen(
        const detail::Instruction& instruction) noexcept
    {
        ExpressionValue condition;
        ExpressionValue trigger;
        if (!pop(condition) || !popTrigger(instruction, trigger))
            return false;
        if (condition.domain != ValueDomain::Scalar || !std::isfinite(condition.value))
            return fail(Diagnostic::Code::InvalidArgument, instruction.range);
        auto* cell = stateCell(instruction);
        if (cell == nullptr)
            return false;
        const auto high = trigger.value > 0.0;
        if (high && !cell->auxiliary)
            cell->value = {condition.value > 0.0 ? 1.0 : 0.0, ValueDomain::Scalar};
        cell->initialized = true;
        cell->auxiliary = high;
        ++instructionIndex_;
        return push({high && cell->value.value > 0.0 ? trigger.value : 0.0, ValueDomain::Scalar});
    }

    // trigger durationBetweenTriggers: n is the number of seconds the last
    // n triggers span, durationBetweenTriggers the last one, and
    // durationOfGate the seconds the last gate stayed on, not counting the
    // -1 of a stolen voice. Ten trigger times are kept.
    [[nodiscard]] bool executeTriggerDuration(
        const detail::Instruction& instruction) noexcept
    {
        ExpressionValue span;
        ExpressionValue trigger;
        if (!pop(span) || !popTrigger(instruction, trigger))
            return false;
        constexpr std::size_t kept = 10;
        std::uint64_t count = 1;
        if (instruction.number == 0.0
            && !countArgument(instruction, span, 1.0, static_cast<double>(kept - 1), count))
        {
            return false;
        }
        auto* cell = stateCell(instruction);
        if (cell == nullptr)
            return false;
        if (instruction.referenceIndex + 5u >= candidateState_->slotCount)
            return fail(Diagnostic::Code::MissingState, instruction.range);
        std::uint64_t tick = 0;
        if (!currentTick(instruction, *cell, tick))
            return false;
        const auto timeAt = [&](std::size_t index) -> std::uint64_t& {
            auto& holder = cell[1 + index / 2];
            return index % 2 == 0 ? holder.startTimeTick : holder.lastTimeTick;
        };
        if (!cell->initialized)
            cell->value = {0.0, ValueDomain::Scalar};
        const auto high = trigger.value > 0.0;
        if (instruction.number == 0.0)
        {
            if (high && !cell->auxiliary)
            {
                const auto seen = cell->startTimeTick;
                timeAt(static_cast<std::size_t>(seen % kept)) = tick;
                if (seen >= count)
                {
                    const auto earlier = timeAt(static_cast<std::size_t>((seen - count) % kept));
                    cell->value = {static_cast<double>(tick - earlier) / 1000.0, ValueDomain::Scalar};
                }
                cell->startTimeTick = seen + 1;
            }
        }
        else
        {
            if (high && !cell->auxiliary)
                timeAt(0) = tick;
            else if (!high && cell->auxiliary)
                cell->value = {static_cast<double>(tick - timeAt(0)) / 1000.0, ValueDomain::Scalar};
        }
        cell->initialized = true;
        cell->auxiliary = high;
        cell->lastTimeTick = tick;
        ++instructionIndex_;
        return push(cell->value);
    }

    // receiver hasChangedReset: reset is 1 once the receiver turns from 0
    // to nonzero, until the reset becomes positive.
    [[nodiscard]] bool executeChangedReset(
        const detail::Instruction& instruction) noexcept
    {
        ExpressionValue reset;
        ExpressionValue receiver;
        if (!pop(reset) || !popTrigger(instruction, receiver))
            return false;
        if (reset.domain != ValueDomain::Scalar || !std::isfinite(reset.value))
            return fail(Diagnostic::Code::InvalidArgument, instruction.range);
        auto* cell = stateCell(instruction);
        if (cell == nullptr)
            return false;
        if (instruction.referenceIndex + 1u >= candidateState_->slotCount)
            return fail(Diagnostic::Code::MissingState, instruction.range);
        auto* resetCell = cell + 1;
        if (!cell->initialized)
            cell->value = {0.0, ValueDomain::Scalar};
        const auto resetting = reset.value > 0.0;
        const auto changed = receiver.value != 0.0;
        if (resetting && !resetCell->auxiliary)
            cell->value.value = 0.0;
        if (changed && !cell->auxiliary)
            cell->value.value = 1.0;
        cell->initialized = true;
        cell->auxiliary = changed;
        resetCell->auxiliary = resetting;
        ++instructionIndex_;
        return push(cell->value);
    }

    // trigger sustainBy: pedal holds the trigger that was on when the
    // pedal went down until the pedal comes up, as a sostenuto pedal does
    //.
    [[nodiscard]] bool executeSustainBy(
        const detail::Instruction& instruction) noexcept
    {
        ExpressionValue pedal;
        ExpressionValue trigger;
        if (!pop(pedal) || !popTrigger(instruction, trigger))
            return false;
        if (pedal.domain != ValueDomain::Scalar || !std::isfinite(pedal.value))
            return fail(Diagnostic::Code::InvalidArgument, instruction.range);
        auto* cell = stateCell(instruction);
        if (cell == nullptr)
            return false;
        if (instruction.referenceIndex + 1u >= candidateState_->slotCount)
            return fail(Diagnostic::Code::MissingState, instruction.range);
        auto* pedalCell = cell + 1;
        const auto down = pedal.value > 0.0;
        if (down && !pedalCell->auxiliary)
        {
            cell->value = {trigger.value, ValueDomain::Scalar};
            cell->auxiliary = trigger.value > 0.0;
        }
        if (!down)
            cell->auxiliary = false;
        cell->initialized = true;
        pedalCell->auxiliary = down;
        ++instructionIndex_;
        return push({cell->auxiliary && trigger.value <= 0.0 ? cell->value.value : trigger.value,
            ValueDomain::Scalar});
    }

    // trigger nextChaotic: r steps the logistic map x = (3 + r) x (1 - x)
    // on each trigger, from 0.5 or from seed:, and reset: puts a uniform
    // random x in whenever it turns positive.
    [[nodiscard]] bool executeChaotic(
        const detail::Instruction& instruction) noexcept
    {
        ExpressionValue reset;
        ExpressionValue seed;
        ExpressionValue rate;
        ExpressionValue trigger;
        if (!pop(reset) || !pop(seed) || !pop(rate) || !popTrigger(instruction, trigger))
            return false;
        if (rate.domain != ValueDomain::Scalar || !std::isfinite(rate.value)
            || seed.domain != ValueDomain::Scalar || !std::isfinite(seed.value)
            || reset.domain != ValueDomain::Scalar || !std::isfinite(reset.value))
        {
            return fail(Diagnostic::Code::InvalidArgument, instruction.range);
        }
        auto* cell = stateCell(instruction);
        if (cell == nullptr)
            return false;
        if (instruction.referenceIndex + 1u >= candidateState_->slotCount)
            return fail(Diagnostic::Code::MissingState, instruction.range);
        auto* resetCell = cell + 1;
        if (!cell->initialized)
        {
            cell->value = {seed.value, ValueDomain::Scalar};
            resetCell->value = {randomSeedFor(instruction), ValueDomain::Scalar};
        }
        const auto resetting = reset.value > 0.0;
        if (resetting && !resetCell->auxiliary)
        {
            resetCell->value.value = nextRandomState(resetCell->value.value);
            cell->value.value = resetCell->value.value / 4294967296.0;
        }
        const auto high = trigger.value > 0.0;
        if (high && !cell->auxiliary)
            cell->value.value = (3.0 + rate.value) * cell->value.value * (1.0 - cell->value.value);
        cell->initialized = true;
        cell->auxiliary = high;
        resetCell->auxiliary = resetting;
        ++instructionIndex_;
        return checked(cell->value, instruction.range);
    }

    // receiver swarmFollowFromPosition: p velocity: v acceleration: a
    // friction: f follows the receiver as a bee follows the queen. The
    // motion is this: the bee starts
    // at p moving at v a second, is drawn toward the queen at a a second a
    // second for each unit between them, and friction takes f of its
    // velocity away each second, in steps of a millisecond.
    [[nodiscard]] bool executeSwarmFollow(
        const detail::Instruction& instruction) noexcept
    {
        ExpressionValue friction;
        ExpressionValue acceleration;
        ExpressionValue velocity;
        ExpressionValue position;
        ExpressionValue queen;
        if (!pop(friction) || !pop(acceleration) || !pop(velocity) || !pop(position) || !pop(queen))
            return false;
        for (const auto* given : {&friction, &acceleration, &velocity})
        {
            if (given->domain != ValueDomain::Scalar || !std::isfinite(given->value))
                return fail(Diagnostic::Code::InvalidArgument, instruction.range);
        }
        if ((position.domain != ValueDomain::Scalar && position.domain != queen.domain)
            || !std::isfinite(position.value) || !std::isfinite(queen.value))
            return fail(Diagnostic::Code::InvalidArgument, instruction.range);
        auto* cell = stateCell(instruction);
        if (cell == nullptr)
            return false;
        if (instruction.referenceIndex + 1u >= candidateState_->slotCount)
            return fail(Diagnostic::Code::MissingState, instruction.range);
        auto* motion = cell + 1;
        std::uint64_t tick = 0;
        if (!currentTick(instruction, *cell, tick))
            return false;
        if (!cell->initialized)
        {
            cell->value = {position.value, queen.domain};
            motion->value = {velocity.value, ValueDomain::Scalar};
            cell->lastTimeTick = tick;
        }
        if (cell->value.domain != queen.domain)
            return fail(Diagnostic::Code::UnitMismatch, instruction.range);
        constexpr double step = 0.001;
        auto place = cell->value.value;
        auto speed = motion->value.value;
        for (auto steps = std::min<std::uint64_t>(tick - cell->lastTimeTick, 60'000); steps > 0; --steps)
        {
            speed += acceleration.value * (queen.value - place) * step;
            speed -= friction.value * speed * step;
            place += speed * step;
        }
        if (!std::isfinite(place) || !std::isfinite(speed))
            return fail(Diagnostic::Code::NonFiniteResult, instruction.range);
        cell->value.value = place;
        motion->value.value = speed;
        cell->initialized = true;
        motion->initialized = true;
        cell->lastTimeTick = tick;
        ++instructionIndex_;
        return checked(cell->value, instruction.range);
    }

    // receiver neg: n zero: z pos: p initially: i is i until the receiver
    // first changes, then the branch its sign selects.
    [[nodiscard]] bool executeInitially(
        const detail::Instruction& instruction) noexcept
    {
        ExpressionValue initial;
        ExpressionValue positive;
        ExpressionValue zero;
        ExpressionValue negative;
        ExpressionValue receiver;
        if (!pop(initial) || !pop(positive) || !pop(zero) || !pop(negative) || !pop(receiver))
            return false;
        if (!std::isfinite(receiver.value))
            return fail(Diagnostic::Code::InvalidArgument, instruction.range);
        auto* cell = stateCell(instruction);
        if (cell == nullptr)
            return false;
        if (!cell->initialized)
        {
            cell->value = receiver;
            cell->auxiliary = false;
        }
        else if (receiver.value != cell->value.value)
            cell->auxiliary = true;
        cell->initialized = true;
        ++instructionIndex_;
        if (!cell->auxiliary)
            return checked(initial, instruction.range);
        return checked(receiver.value < 0.0 ? negative : receiver.value > 0.0 ? positive : zero, instruction.range);
    }

    // trigger trackAndHold: value follows the value while the trigger is on
    // and holds it while it is off.
    [[nodiscard]] bool executeTrackHold(
        const detail::Instruction& instruction) noexcept
    {
        ExpressionValue value;
        ExpressionValue trigger;
        if (!pop(value) || !popTrigger(instruction, trigger))
            return false;
        auto* cell = stateCell(instruction);
        if (cell == nullptr)
            return false;
        if (cell->initialized && cell->value.domain != value.domain)
            return fail(Diagnostic::Code::UnitMismatch, instruction.range);
        if (!cell->initialized)
            cell->value = {0.0, value.domain};
        if (trigger.value > 0.0)
            cell->value = value;
        cell->initialized = true;
        ++instructionIndex_;
        return checked(cell->value, instruction.range);
    }

    [[nodiscard]] ExpressionStateCell* stateCell(
        const detail::Instruction& instruction) noexcept
    {
        if (state_ == nullptr
            || instruction.referenceIndex >= storage_.stateSlotCount
            || candidateState_ == nullptr
            || instruction.referenceIndex >= candidateState_->slotCount
            || instruction.referenceIndex >= candidateState_->cells.size())
        {
            (void) fail(Diagnostic::Code::MissingState, instruction.range);
            return nullptr;
        }
        return &candidateState_->cells[instruction.referenceIndex];
    }

    [[nodiscard]] bool executePrevious(
        const detail::Instruction& instruction) noexcept
    {
        ExpressionValue initial;
        ExpressionValue value;
        if (!pop(initial) || !pop(value)
            || !makeDomainsCompatible(value, initial, instruction.range))
        {
            return false;
        }
        auto* cell = stateCell(instruction);
        if (cell == nullptr)
            return false;
        auto output = initial;
        if (cell->initialized)
        {
            output = cell->value;
            if (!makeDomainsCompatible(output, value, instruction.range))
                return false;
        }
        cell->value = value;
        cell->initialized = true;
        ++instructionIndex_;
        return checked(output, instruction.range);
    }

    [[nodiscard]] bool executeAccumulate(
        const detail::Instruction& instruction) noexcept
    {
        ExpressionValue initial;
        ExpressionValue reset;
        ExpressionValue delta;
        if (!pop(initial) || !pop(reset) || !pop(delta)
            || reset.domain != ValueDomain::Scalar
            || !makeDomainsCompatible(delta, initial, instruction.range))
        {
            return reset.domain == ValueDomain::Scalar
                ? false
                : fail(Diagnostic::Code::UnitMismatch, instruction.range);
        }
        auto* cell = stateCell(instruction);
        if (cell == nullptr)
            return false;
        auto result = initial;
        if (reset.value == 0.0)
        {
            auto prior = cell->initialized ? cell->value : initial;
            if (!makeDomainsCompatible(prior, delta, instruction.range))
                return false;
            result = {prior.value + delta.value, prior.domain};
        }
        if (!std::isfinite(result.value))
            return fail(Diagnostic::Code::NonFiniteResult, instruction.range);
        cell->value = result;
        cell->initialized = true;
        ++instructionIndex_;
        return push(result);
    }

    [[nodiscard]] bool executeSampleHold(
        const detail::Instruction& instruction) noexcept
    {
        ExpressionValue initial;
        ExpressionValue trigger;
        ExpressionValue value;
        if (!pop(initial) || !pop(trigger) || !pop(value)
            || trigger.domain != ValueDomain::Scalar
            || !makeDomainsCompatible(value, initial, instruction.range))
        {
            return trigger.domain == ValueDomain::Scalar
                ? false
                : fail(Diagnostic::Code::UnitMismatch, instruction.range);
        }
        auto* cell = stateCell(instruction);
        if (cell == nullptr)
            return false;
        const auto triggerHigh = trigger.value != 0.0;
        auto held = cell->initialized ? cell->value : initial;
        if (cell->initialized
            && !makeDomainsCompatible(held, value, instruction.range))
        {
            return false;
        }
        if (triggerHigh && (!cell->initialized || !cell->auxiliary))
            held = value;
        cell->value = held;
        cell->initialized = true;
        cell->auxiliary = triggerHigh;
        ++instructionIndex_;
        return checked(held, instruction.range);
    }

    [[nodiscard]] bool executeRamp(
        const detail::Instruction& instruction) noexcept
    {
        ExpressionValue duration;
        ExpressionValue trigger;
        if (!pop(duration) || !pop(trigger))
            return false;
        if (trigger.domain != ValueDomain::Scalar
            || !convertToExpected(
                duration, ValueDomain::Seconds, instruction.range))
            return fail(Diagnostic::Code::UnitMismatch, instruction.range);
        if (!std::isfinite(duration.value) || duration.value <= 0.0)
            return fail(Diagnostic::Code::InvalidArgument, instruction.range);
        if (!std::isfinite(context_.sampleRate)
            || context_.sampleRate <= 0.0)
            return fail(Diagnostic::Code::MissingSampleRate, instruction.range);
        constexpr double maximumExactTick = 9007199254740992.0;
        const auto tickValue = std::floor(
            static_cast<double>(context_.elapsedSampleFrame)
                * 1000.0 / context_.sampleRate);
        if (!std::isfinite(tickValue)
            || tickValue < 0.0
            || tickValue > maximumExactTick)
            return fail(Diagnostic::Code::InvalidArgument, instruction.range);
        auto tick = static_cast<std::uint64_t>(tickValue);
        auto* cell = stateCell(instruction);
        if (cell == nullptr)
            return false;
        leaveOutSkippedTime(*cell, tick);
        if (cell->initialized && tick < cell->lastTimeTick)
            return fail(Diagnostic::Code::InvalidArgument, instruction.range);

        const auto high = trigger.value > 0.0;
        // ramp rests at 1 until its receiver first becomes positive.
        double output = cell->initialized ? cell->value.value
            : context_.rampsStartAtFirstValue ? 0.0 : 1.0;
        if (high && (!cell->initialized || !cell->auxiliary))
        {
            cell->startTimeTick = tick;
            output = 0.0;
        }
        // Once triggered it runs on to its end, where
        // projects before format 103 hold it while the trigger is off.
        else if (high || (!context_.rampsHoldUntriggered && !context_.rampsStartAtFirstValue && cell->initialized
                     && cell->value.value < 1.0))
        {
            const auto elapsedSeconds = static_cast<double>(
                tick - cell->startTimeTick) / 1000.0;
            output = std::min(1.0, elapsedSeconds / duration.value);
        }
        if (!std::isfinite(output))
            return fail(Diagnostic::Code::NonFiniteResult, instruction.range);
        cell->value = {output, ValueDomain::Scalar};
        cell->initialized = true;
        cell->auxiliary = high;
        cell->lastTimeTick = tick;
        ++instructionIndex_;
        return push(cell->value);
    }

    // A Val time function in a branch not
    // taken stops, and carries on from where it left off when the branch
    // is next taken. `tick`, the Sound's, becomes the cell's own, without
    // the time it missed: from its last evaluation to the expression's
    // last before this one.
    void leaveOutSkippedTime(ExpressionStateCell& cell, std::uint64_t& tick) const noexcept
    {
        if (!cell.initialized)
            return;
        if (pausesSkippedTime_ && candidateState_ != nullptr)
        {
            const auto last = cell.lastTimeTick + cell.pausedTicks;
            const auto previous = candidateState_->lastEvaluationTick;
            if (previous > last + 1 && previous - 1 <= tick)
                cell.pausedTicks += previous - 1 - last;
        }
        tick = tick >= cell.pausedTicks ? tick - cell.pausedTicks : 0;
    }

    // The millisecond tick of the running Sound, as ramp: uses, checked
    // to stay monotonic for the instruction's state cell.
    [[nodiscard]] bool currentTick(
        const detail::Instruction& instruction,
        ExpressionStateCell& cell,
        std::uint64_t& tick) noexcept
    {
        if (!std::isfinite(context_.sampleRate)
            || context_.sampleRate <= 0.0)
            return fail(Diagnostic::Code::MissingSampleRate, instruction.range);
        constexpr double maximumExactTick = 9007199254740992.0;
        const auto tickValue = std::floor(
            static_cast<double>(context_.elapsedSampleFrame)
                * 1000.0 / context_.sampleRate);
        if (!std::isfinite(tickValue)
            || tickValue < 0.0
            || tickValue > maximumExactTick)
            return fail(Diagnostic::Code::InvalidArgument, instruction.range);
        tick = static_cast<std::uint64_t>(tickValue);
        leaveOutSkippedTime(cell, tick);
        if (cell.initialized && tick < cell.lastTimeTick)
            return fail(Diagnostic::Code::InvalidArgument, instruction.range);
        return true;
    }

    [[nodiscard]] bool popDuration(
        const detail::Instruction& instruction,
        double& seconds,
        bool zeroIsOneTick = false) noexcept
    {
        ExpressionValue duration;
        if (!pop(duration)
            || !convertToExpected(duration, ValueDomain::Seconds, instruction.range))
        {
            return fail(Diagnostic::Code::UnitMismatch, instruction.range);
        }
        if (zeroIsOneTick && duration.value == 0.0)
        {
            seconds = 0.001;
            return true;
        }
        if (!std::isfinite(duration.value) || duration.value <= 0.0)
            return fail(Diagnostic::Code::InvalidArgument, instruction.range);
        seconds = duration.value;
        return true;
    }

    // Val's ramp family: from a trigger's
    // rising edge a ramp goes 0 to 1, a full ramp -1 to 1 and a triangle
    // 0 to 1 to 0 over the duration; repeating forms start again for as
    // long as the trigger stays on. A ramp or full ramp, once triggered,
    // runs on to its end when the trigger falls,
    // where projects before format 103 hold it; otherwise without a trigger
    // the value holds.
    [[nodiscard]] bool executeTimeFunction(
        const detail::Instruction& instruction) noexcept
    {
        // A duration of 0, a fader at its bottom times a duration, goes as
        // fast as it can, a millisecond tick.
        double duration = 0.0;
        ExpressionValue trigger;
        if (!popDuration(instruction, duration, true) || !pop(trigger))
            return false;
        if (trigger.domain != ValueDomain::Scalar)
            return fail(Diagnostic::Code::UnitMismatch, instruction.range);
        auto* cell = stateCell(instruction);
        std::uint64_t tick = 0;
        if (cell == nullptr || !currentTick(instruction, *cell, tick))
            return false;
        const auto variant = static_cast<int>(instruction.number);
        const auto shape = variant % 4;
        const auto repeating = variant >= 4 && variant != localTimeVariant && variant != wholeSoundTimeVariant
            && variant != controlledWholeSoundTimeVariant;
        const auto shaped = [shape](double phase) {
            return shape == 1 ? 2.0 * phase - 1.0
                : shape == 2 ? (phase < 0.5 ? 2.0 * phase : 2.0 - 2.0 * phase)
                : phase;
        };
        const auto high = trigger.value > 0.0;
        // The ramps rest at 1 until their receiver first becomes positive
        //; a triangle starts at its first value.
        const auto rests = (shape == 0 || shape == 1) && !context_.rampsStartAtFirstValue;
        auto output = cell->initialized ? cell->value.value : rests ? 1.0 : shaped(0.0);
        if (variant == localTimeVariant)
        {
            // The expression clock counts from the Sound's start, as the
            // TimeControls and TimeStoppers around it let time pass.
            output = std::isfinite(context_.controlledLocalTimeSeconds)
                ? std::floor(context_.controlledLocalTimeSeconds * 1000.0 + 1.0e-9) / 1000.0
                : static_cast<double>(tick) / 1000.0;
        }
        else if (variant == controlledWholeSoundTimeVariant && std::isfinite(context_.controlledTimeSeconds))
            output = std::floor(context_.controlledTimeSeconds * 1000.0 + 1.0e-9) / 1000.0;
        else if (variant == wholeSoundTimeVariant || variant == controlledWholeSoundTimeVariant)
        {
            // The whole Sound's clock, from before the expression's own
            // Sound started, and not paused in a branch not taken.
            output = std::floor(static_cast<double>(context_.elapsedSampleFrame + context_.soundStartSampleFrame)
                         * 1000.0 / context_.sampleRate)
                / 1000.0;
        }
        else if (high && (!cell->initialized || !cell->auxiliary))
        {
            cell->startTimeTick = tick;
            output = shaped(0.0);
        }
        else if (high || (!repeating && (shape == 0 || shape == 1) && !context_.rampsHoldUntriggered
                     && cell->initialized && cell->value.value < 1.0 && !context_.rampsStartAtFirstValue))
        {
            const auto elapsed = static_cast<double>(
                tick - cell->startTimeTick) / 1000.0;
            output = shaped(repeating
                ? std::fmod(elapsed, duration) / duration
                : std::min(1.0, elapsed / duration));
        }
        if (!std::isfinite(output))
            return fail(Diagnostic::Code::NonFiniteResult, instruction.range);
        cell->value = {output, ValueDomain::Scalar};
        cell->initialized = true;
        cell->auxiliary = high;
        cell->lastTimeTick = tick;
        ++instructionIndex_;
        return push(cell->value);
    }

    // trigger bpm: rate dutyCycle: duty: while the
    // trigger is on, 1 for the duty cycle of each beat and 0 for the rest,
    // starting with a beat at the trigger's rising edge.
    [[nodiscard]] bool executeMetronome(
        const detail::Instruction& instruction) noexcept
    {
        ExpressionValue duty;
        ExpressionValue rate;
        ExpressionValue trigger;
        if (!pop(duty) || !pop(rate) || !pop(trigger))
            return false;
        if (trigger.domain != ValueDomain::Scalar
            || rate.domain != ValueDomain::Scalar
            || duty.domain != ValueDomain::Scalar)
        {
            return fail(Diagnostic::Code::UnitMismatch, instruction.range);
        }
        if (!std::isfinite(rate.value)
            || !std::isfinite(duty.value) || duty.value < 0.0 || duty.value > 1.0)
        {
            return fail(Diagnostic::Code::InvalidArgument, instruction.range);
        }
        auto* cell = stateCell(instruction);
        std::uint64_t tick = 0;
        if (cell == nullptr || !currentTick(instruction, *cell, tick))
            return false;
        // A rate of 0 beats a minute, the bottom of a fader times a tempo,
        // never beats.
        const auto high = trigger.value > 0.0 && rate.value > 0.0;
        if (high && (!cell->initialized || !cell->auxiliary))
            cell->startTimeTick = tick;
        auto output = 0.0;
        if (high)
        {
            // In milliseconds, the ticks counted exactly.
            const auto period = 60000.0 / rate.value;
            const auto phase = std::fmod(static_cast<double>(tick - cell->startTimeTick), period);
            const auto on = duty.value * period;
            // A Val bpm: beat is -1 for its first five milliseconds of
            // time on, to steal its voice as !KeyDown does, then 1.
            output = phase >= on                                          ? 0.0
                : instruction.number > 0.0 && phase < std::min(5.0, on) ? -1.0
                                                                          : 1.0;
        }
        cell->value = {output, ValueDomain::Scalar};
        cell->initialized = true;
        cell->auxiliary = high;
        cell->lastTimeTick = tick;
        ++instructionIndex_;
        return push(cell->value);
    }

    // The MIDI timing clock's trigger, which rises
    // once for each message the renderer counts in
    // midiTimingClockCountEventName and falls at the next millisecond it is
    // evaluated in. A trigger already high when a message comes (the stand-in
    // a beat's tick, or two messages within one evaluation) falls first and
    // rises at the next evaluation, so that every message gives a rise.
    // Before the first message, and where no renderer keeps the count,
    // !TimingClock is its stand-in and `MIDITimingClock is 0; as the
    // stand-in does, it also rises when its Sound starts while the clock
    // runs. The first cell
    // keeps the count last seen, whether a rise waits and when it rose; the
    // second, the value last given.
    [[nodiscard]] bool executeMidiTimingClock(
        const detail::Instruction& instruction) noexcept
    {
        ExpressionValue standIn;
        if (!pop(standIn))
            return false;
        if (standIn.domain != ValueDomain::Scalar)
            return fail(Diagnostic::Code::UnitMismatch, instruction.range);
        auto* cell = stateCell(instruction);
        if (cell == nullptr || instruction.referenceIndex + 1U >= candidateState_->slotCount)
            return fail(Diagnostic::Code::MissingState, instruction.range);
        auto& given = candidateState_->cells[instruction.referenceIndex + 1U];
        std::uint64_t tick = 0;
        if (!currentTick(instruction, *cell, tick))
            return false;
        ExpressionValue count;
        const auto counted = context_.resolveEvent != nullptr
            && context_.resolveEvent(context_.user, midiTimingClockCountEventName, count)
            && count.domain == ValueDomain::Scalar && std::isfinite(count.value) && count.value > 0.0;
        const auto previous = given.initialized ? given.value.value : 0.0;
        auto output = 0.0;
        if (!counted)
        {
            output = instruction.number > 0.0 ? standIn.value : 0.0;
            cell->value = {0.0, ValueDomain::Scalar};
            cell->auxiliary = false;
        }
        else if (!cell->initialized || count.value != cell->value.value)
        {
            cell->value = {count.value, ValueDomain::Scalar};
            cell->auxiliary = previous > 0.0;
            if (!cell->auxiliary)
            {
                cell->startTimeTick = tick;
                output = 1.0;
            }
        }
        else if (cell->auxiliary)
        {
            cell->auxiliary = false;
            cell->startTimeTick = tick;
            output = 1.0;
        }
        else if (previous > 0.0 && tick == cell->startTimeTick)
            output = 1.0;
        if (!std::isfinite(output))
            return fail(Diagnostic::Code::NonFiniteResult, instruction.range);
        cell->initialized = true;
        cell->lastTimeTick = tick;
        given.value = {output, ValueDomain::Scalar};
        given.initialized = true;
        given.lastTimeTick = tick;
        ++instructionIndex_;
        return push(given.value);
    }

    // A 32-bit linear congruential state, which a double holds exactly.
    [[nodiscard]] static double nextRandomState(double state) noexcept
    {
        return std::fmod(state * 1664525.0 + 1013904223.0, 4294967296.0);
    }

    // A fixed seed per expression slot keeps renders repeatable; the
    // field's salt sets one field's slots apart from another's.
    [[nodiscard]] double randomSeedFor(
        const detail::Instruction& instruction) const noexcept
    {
        const auto mixed = 2654435769ULL * (static_cast<std::uint64_t>(instruction.referenceIndex) + 1ULL)
            + 2246822519ULL * static_cast<std::uint64_t>(state_ == nullptr ? 0U : state_->randomSalt);
        return static_cast<double>(mixed % 4294967296ULL);
    }

    // The number a state stands for: uniform in [-1, 1); normal around 0,
    // a third of the range a standard deviation and clipped to it; or
    // exponential in [0, 24], a mean of 3.
    [[nodiscard]] static double randomValue(double state, double distribution) noexcept
    {
        const auto uniform = state / 4294967296.0;
        if (distribution == 1.0)
        {
            constexpr double twoPi = 6.283185307179586476925286766559;
            const auto angle = nextRandomState(state) / 4294967296.0;
            const auto normal = std::sqrt(-2.0 * std::log(1.0 - uniform)) * std::cos(twoPi * angle);
            return std::clamp(normal / 3.0, -1.0, 1.0);
        }
        if (distribution == 2.0)
            return std::min(-3.0 * std::log(1.0 - uniform), 24.0);
        return uniform * 2.0 - 1.0;
    }

    // interval random draws a new number every interval, and trigger
    // nextRandom one on each rising edge, from a fixed seed per expression
    // slot or the seed nextRandomWithSeed: names; normal, randExp and their
    // next... forms draw from the other distributions.
    [[nodiscard]] bool executeRandom(
        const detail::Instruction& instruction) noexcept
    {
        ExpressionValue receiver;
        ExpressionValue seed;
        const auto interval = instruction.op == detail::InstructionOp::RandomInterval;
        const auto seeded = instruction.op == detail::InstructionOp::NextRandomSeeded;
        double seconds = 0.0;
        if (seeded && !pop(seed))
            return false;
        if (interval ? !popDuration(instruction, seconds) : !pop(receiver))
            return false;
        if (!interval && receiver.domain != ValueDomain::Scalar)
            return fail(Diagnostic::Code::UnitMismatch, instruction.range);
        if (seeded && (seed.domain != ValueDomain::Scalar || !std::isfinite(seed.value)))
            return fail(Diagnostic::Code::InvalidArgument, instruction.range);
        auto* cell = stateCell(instruction);
        std::uint64_t tick = 0;
        if (cell == nullptr || !currentTick(instruction, *cell, tick))
            return false;
        auto draws = 0;
        if (!cell->initialized)
        {
            cell->value = {
                seeded ? nextRandomState(std::fmod(std::fabs(std::trunc(seed.value)), 4294967296.0))
                       : randomSeedFor(instruction),
                ValueDomain::Scalar};
            cell->startTimeTick = tick;
            draws = interval ? 1 : 0;
        }
        if (interval && cell->initialized)
        {
            const auto period = std::max<std::uint64_t>(
                1, static_cast<std::uint64_t>(std::llround(seconds * 1000.0)));
            if (tick - cell->startTimeTick >= period)
            {
                draws = 1;
                cell->startTimeTick += (tick - cell->startTimeTick) / period * period;
            }
        }
        const auto high = !interval && receiver.value > 0.0;
        if (!interval && high && cell->initialized && !cell->auxiliary)
            draws = 1;
        if (!interval && high && !cell->initialized)
            draws = 1;
        if (draws != 0)
            cell->value.value = nextRandomState(cell->value.value);
        const auto drawn = cell->initialized || draws != 0;
        cell->initialized = true;
        cell->auxiliary = high;
        cell->lastTimeTick = tick;
        ++instructionIndex_;
        return push({drawn ? randomValue(cell->value.value, instruction.number) : 0.0,
            ValueDomain::Scalar});
    }

    // value smooth: duration: a new value is reached by
    // a straight line over the duration, starting from where the output
    // is; the first value is taken as it is.
    [[nodiscard]] bool executeSmooth(
        const detail::Instruction& instruction) noexcept
    {
        double duration = 0.0;
        ExpressionValue target;
        if (!popDuration(instruction, duration) || !pop(target))
            return false;
        auto* cell = stateCell(instruction);
        if (cell == nullptr)
            return false;
        auto* goal = cell + 1;
        if (instruction.referenceIndex + 1u >= candidateState_->slotCount)
            return fail(Diagnostic::Code::MissingState, instruction.range);
        std::uint64_t tick = 0;
        if (!currentTick(instruction, *cell, tick))
            return false;
        if (cell->initialized && goal->value.domain != target.domain)
            return fail(Diagnostic::Code::UnitMismatch, instruction.range);
        const auto elapsed = cell->initialized
            ? static_cast<double>(tick - cell->startTimeTick) / 1000.0
            : 0.0;
        const auto progress = std::min(1.0, elapsed / duration);
        const auto current = cell->initialized
            ? cell->value.value + (goal->value.value - cell->value.value) * progress
            : target.value;
        if (!cell->initialized || target.value != goal->value.value)
        {
            cell->value = {current, target.domain};
            cell->startTimeTick = tick;
            goal->value = target;
            goal->initialized = true;
        }
        cell->initialized = true;
        cell->lastTimeTick = tick;
        ++instructionIndex_;
        return checked({current, target.domain}, instruction.range);
    }

    [[nodiscard]] bool executeUnitApplication(
        const detail::Instruction& instruction) noexcept
    {
        ExpressionValue scalar;
        if (!pop(scalar)) return false;
        if (scalar.domain != ValueDomain::Scalar)
        {
            // In Val, hz, nn and s convert what a value measures, and
            // samp counts a time in samples, as in
            // `(1.0 / 120) s samp`; bpm makes a
            // frequency as hz does, 4 s bpm being 15 bpm.
            const auto target = instruction.unit == Unit::Hertz || instruction.unit == Unit::BeatsPerMinute
                ? std::optional{ValueDomain::Hertz}
                : instruction.unit == Unit::NoteNumber
                    ? std::optional{ValueDomain::NoteNumber}
                    : instruction.unit == Unit::Seconds
                        ? std::optional{ValueDomain::Seconds}
                        : instruction.unit == Unit::Samples
                            ? std::optional{ValueDomain::Samples}
                            : std::nullopt;
            if (!usesValMessages(storage_.languageVersion)
                || !target.has_value())
            {
                return fail(Diagnostic::Code::UnitMismatch, instruction.range);
            }
            if (*target == ValueDomain::Samples)
            {
                if (!convertVal(scalar, ValueDomain::Seconds, instruction.range))
                    return false;
                if (!isPositiveFinite(context_.sampleRate))
                    return fail(Diagnostic::Code::MissingSampleRate, instruction.range);
                scalar = {scalar.value * context_.sampleRate, ValueDomain::Samples};
            }
            else if (!convertVal(scalar, *target, instruction.range))
                return false;
            ++instructionIndex_;
            return checked(scalar, instruction.range);
        }
        // Reuse literal conversion, including decibels and tempo-dependent
        // beats, so dynamic units retain the same checked numeric semantics.
        auto literal = instruction;
        literal.number = scalar.value;
        return executeLiteral(literal);
    }

    [[nodiscard]] bool executeLiteral(
        const detail::Instruction& instruction) noexcept
    {
        ExpressionValue value;
        switch (instruction.unit)
        {
        case Unit::None:
            value = {instruction.number, ValueDomain::Scalar};
            break;
        case Unit::Hertz:
            value = {instruction.number, ValueDomain::Hertz};
            break;
        case Unit::Decibels:
            value = {
                std::pow(10.0, instruction.number / 20.0),
                ValueDomain::Scalar};
            break;
        case Unit::Milliseconds:
            value = {instruction.number / 1000.0, ValueDomain::Seconds};
            break;
        case Unit::Microseconds:
            value = {instruction.number / 1000000.0, ValueDomain::Seconds};
            break;
        case Unit::Seconds:
            value = {instruction.number, ValueDomain::Seconds};
            break;
        case Unit::Minutes:
            value = {instruction.number * 60.0, ValueDomain::Seconds};
            break;
        case Unit::Hours:
            value = {instruction.number * 3600.0, ValueDomain::Seconds};
            break;
        case Unit::Days:
            value = {instruction.number * 86400.0, ValueDomain::Seconds};
            break;
        case Unit::NoteNumber:
            value = {instruction.number, ValueDomain::NoteNumber};
            break;
        case Unit::Beats:
            if (!isPositiveFinite(context_.tempoBpm))
            {
                return fail(
                    Diagnostic::Code::MissingTempo,
                    instruction.range);
            }
            value = {
                instruction.number * 60.0 / context_.tempoBpm,
                ValueDomain::Seconds};
            break;
        case Unit::Samples:
            value = {instruction.number, ValueDomain::Samples};
            break;
        case Unit::BeatsPerMinute:
            value = {instruction.number / 60.0, ValueDomain::Hertz};
            break;
        }
        ++instructionIndex_;
        return checked(value, instruction.range);
    }

    [[nodiscard]] bool executeReference(
        const detail::Instruction& instruction,
        ReferenceKind kind) noexcept
    {
        const auto& references = kind == ReferenceKind::Event
            ? storage_.eventReferences
            : kind == ReferenceKind::Variable
                ? storage_.variableReferences
                : storage_.identifiers;
        if (instruction.referenceIndex >= references.size())
        {
            return fail(
                Diagnostic::Code::BudgetExceeded,
                instruction.range);
        }
        const auto resolver = kind == ReferenceKind::Event
            ? context_.resolveEvent
            : kind == ReferenceKind::Variable
                ? context_.resolveVariable
                : context_.resolveIdentifier;
        ExpressionValue value;
        const auto resolved = kind == ReferenceKind::Variable
                && context_.resolveVariableIndex != nullptr
            ? context_.resolveVariableIndex(
                  context_.user,
                  instruction.referenceIndex,
                  value)
            : resolver != nullptr
                && resolver(
                    context_.user,
                    references[instruction.referenceIndex],
                    value);
        if (!resolved)
        {
            return fail(
                kind == ReferenceKind::Event
                    ? Diagnostic::Code::UnknownEvent
                    : kind == ReferenceKind::Variable
                        ? Diagnostic::Code::UnknownVariable
                        : Diagnostic::Code::UnknownIdentifier,
                instruction.range);
        }
        if (!std::isfinite(value.value))
        {
            return fail(
                Diagnostic::Code::NonFiniteResult,
                instruction.range);
        }
        ++instructionIndex_;
        return push(value);
    }

    [[nodiscard]] bool executeNegate(
        const detail::Instruction& instruction) noexcept
    {
        ExpressionValue value;
        if (!pop(value))
            return false;
        value.value = -value.value;
        ++instructionIndex_;
        return checked(value, instruction.range);
    }

    [[nodiscard]] bool executeBinary(
        const detail::Instruction& instruction) noexcept
    {
        ExpressionValue right;
        ExpressionValue left;
        if (!pop(right) || !pop(left))
            return false;
        ExpressionValue result;
        switch (instruction.op)
        {
        case detail::InstructionOp::Add:
        case detail::InstructionOp::Subtract:
            if (!makeDomainsCompatible(left, right, instruction.range))
                return false;
            result = {
                instruction.op == detail::InstructionOp::Subtract
                    ? left.value - right.value
                    : left.value + right.value,
                left.domain};
            break;
        case detail::InstructionOp::Multiply:
            if (left.domain == ValueDomain::Scalar
                && right.domain == ValueDomain::Scalar)
            {
                result = {
                    left.value * right.value,
                    ValueDomain::Scalar};
            }
            else if (left.domain == ValueDomain::Scalar)
                result = {left.value * right.value, right.domain};
            else if (right.domain == ValueDomain::Scalar)
                result = {left.value * right.value, left.domain};
            else
                return fail(Diagnostic::Code::UnitMismatch, instruction.range);
            break;
        case detail::InstructionOp::Divide:
            // A number other than 0 divided by 0 is
            // infinite under the project's choice; 0 / 0 is undefined.
            if (right.value == 0.0
                && !(context_.inverseOfZeroIsInfinite && left.value != 0.0 && !std::isnan(left.value)))
            {
                return fail(
                    Diagnostic::Code::DivisionByZero,
                    instruction.range);
            }
            // Val divides a frequency or time by a pitch, frequency or
            // period as by one of its own kind (!Pitch hz / 3 b).
            if (usesValMessages(storage_.languageVersion)
                && left.domain != right.domain
                && left.domain != ValueDomain::Scalar
                && left.domain != ValueDomain::NoteNumber
                && right.domain != ValueDomain::Scalar)
            {
                if (left.domain == ValueDomain::Samples
                    && !normalizeTime(left, instruction.range))
                {
                    return false;
                }
                if (!convertVal(right, left.domain, instruction.range))
                    return false;
            }
            if (left.domain == ValueDomain::Scalar
                && right.domain == ValueDomain::Scalar)
            {
                result = {
                    left.value / right.value,
                    ValueDomain::Scalar};
            }
            else if (right.domain == ValueDomain::Scalar)
                result = {left.value / right.value, left.domain};
            // A frequency, time or pitch divided by another of its kind is
            // their ratio, as 4 c sharp hz / 4 c hz is and
            // !KeyNumber nn / 127 nn is.
            else if (left.domain == right.domain)
            {
                result = {left.value / right.value, ValueDomain::Scalar};
            }
            else if (left.domain == ValueDomain::Scalar
                && right.domain == ValueDomain::Seconds)
            {
                result = {left.value / right.value, ValueDomain::Hertz};
            }
            else if (left.domain == ValueDomain::Scalar
                && right.domain == ValueDomain::Hertz)
            {
                result = {left.value / right.value, ValueDomain::Seconds};
            }
            else
                return fail(Diagnostic::Code::UnitMismatch, instruction.range);
            break;
        case detail::InstructionOp::Remainder:
            if (left.domain != ValueDomain::Scalar
                || right.domain != ValueDomain::Scalar)
            {
                return fail(
                    Diagnostic::Code::UnitMismatch,
                    instruction.range);
            }
            if (right.value == 0.0)
            {
                return fail(
                    Diagnostic::Code::DivisionByZero,
                    instruction.range);
            }
            result = {
                std::fmod(left.value, right.value),
                ValueDomain::Scalar};
            if (result.value == 0.0)
                result.value = 0.0;
            break;
        case detail::InstructionOp::Power:
            if (left.domain != ValueDomain::Scalar
                || right.domain != ValueDomain::Scalar)
            {
                return fail(Diagnostic::Code::UnitMismatch, instruction.range);
            }
            result = {std::pow(left.value, right.value), ValueDomain::Scalar};
            break;
        case detail::InstructionOp::IntegerDivide:
        case detail::InstructionOp::Modulo:
        {
            // Val's // divides and rounds toward negative infinity, and
            // its \\ takes the sign of the divisor.
            if (left.domain != right.domain
                && right.domain != ValueDomain::Scalar)
            {
                return fail(Diagnostic::Code::UnitMismatch, instruction.range);
            }
            if (right.value == 0.0)
                return fail(Diagnostic::Code::DivisionByZero, instruction.range);
            if (instruction.op == detail::InstructionOp::IntegerDivide)
            {
                result = {
                    std::floor(left.value / right.value),
                    left.domain == right.domain ? ValueDomain::Scalar : left.domain};
            }
            else
            {
                result = {
                    left.value - right.value * std::floor(left.value / right.value),
                    left.domain};
                if (result.value == 0.0)
                    result.value = 0.0;
            }
            break;
        }
        default:
            return false;
        }
        ++instructionIndex_;
        return checked(result, instruction.range);
    }

    [[nodiscard]] bool executeComparison(
        const detail::Instruction& instruction) noexcept
    {
        ExpressionValue right;
        ExpressionValue left;
        if (!pop(right) || !pop(left))
            return false;
        if (usesValMessages(storage_.languageVersion)
            && left.domain != right.domain)
        {
            // Val compares a plain number with a pitch or frequency
            // by value, as in !KeyNumber eq: 4 c, and converts between
            // pitches, frequencies and periods.
            if (left.domain == ValueDomain::Scalar)
                left.domain = right.domain;
            else if (right.domain == ValueDomain::Scalar)
                right.domain = left.domain;
            else if (!convertVal(right, left.domain, instruction.range))
                return false;
        }
        if (!makeDomainsCompatible(left, right, instruction.range))
            return false;
        bool comparison = false;
        switch (instruction.op)
        {
        case detail::InstructionOp::Less:
            comparison = left.value < right.value;
            break;
        case detail::InstructionOp::LessEqual:
            comparison = left.value <= right.value;
            break;
        case detail::InstructionOp::Greater:
            comparison = left.value > right.value;
            break;
        case detail::InstructionOp::GreaterEqual:
            comparison = left.value >= right.value;
            break;
        case detail::InstructionOp::Equal:
            comparison = left.value == right.value;
            break;
        case detail::InstructionOp::NotEqual:
            comparison = left.value != right.value;
            break;
        default:
            return false;
        }
        ++instructionIndex_;
        return push({comparison ? 1.0 : 0.0, ValueDomain::Scalar});
    }

    [[nodiscard]] bool executeUnaryFunction(
        const detail::Instruction& instruction) noexcept
    {
        ExpressionValue value;
        if (!pop(value))
            return false;
        switch (instruction.op)
        {
        case detail::InstructionOp::Abs:
            value.value = std::abs(value.value);
            break;
        case detail::InstructionOp::Sqrt:
            if (value.domain != ValueDomain::Scalar)
                return fail(Diagnostic::Code::UnitMismatch, instruction.range);
            if (value.value < 0.0)
                return fail(Diagnostic::Code::InvalidArgument, instruction.range);
            value.value = std::sqrt(value.value);
            break;
        case detail::InstructionOp::Log:
            if (value.domain != ValueDomain::Scalar)
                return fail(Diagnostic::Code::UnitMismatch, instruction.range);
            if (value.value <= 0.0)
                return fail(Diagnostic::Code::InvalidArgument, instruction.range);
            value.value = std::log(value.value);
            break;
        case detail::InstructionOp::Exp:
            if (value.domain != ValueDomain::Scalar)
                return fail(Diagnostic::Code::UnitMismatch, instruction.range);
            value.value = std::exp(value.value);
            break;
        default:
            if (!executeValUnary(instruction, value))
                return false;
            break;
        }
        ++instructionIndex_;
        return checked(value, instruction.range);
    }

    // Val's unary messages.
    [[nodiscard]] bool executeValUnary(
        const detail::Instruction& instruction,
        ExpressionValue& value) noexcept
    {
        constexpr double pi = 3.14159265358979323846;
        const auto requireScalar = [&] {
            return value.domain == ValueDomain::Scalar
                || fail(Diagnostic::Code::UnitMismatch, instruction.range);
        };
        switch (instruction.op)
        {
        case detail::InstructionOp::Log10:
        case detail::InstructionOp::Log2:
            if (!requireScalar())
                return false;
            if (value.value <= 0.0)
                return fail(Diagnostic::Code::InvalidArgument, instruction.range);
            value.value = instruction.op == detail::InstructionOp::Log10
                ? std::log10(value.value)
                : std::log2(value.value);
            return true;
        case detail::InstructionOp::Exp2:
            if (!requireScalar())
                return false;
            value.value = std::exp2(value.value);
            return true;
        case detail::InstructionOp::Squared:
            if (!requireScalar())
                return false;
            value.value *= value.value;
            return true;
        case detail::InstructionOp::Reciprocal:
            // The inverse of a frequency is its period, and back.
            if (value.domain == ValueDomain::Samples && !normalizeTime(value, instruction.range))
                return false;
            if (value.domain == ValueDomain::NoteNumber
                && !convertVal(value, ValueDomain::Hertz, instruction.range))
            {
                return false;
            }
            // The inverse of 0 is infinity under the
            // project's choice.
            if (value.value == 0.0 && !context_.inverseOfZeroIsInfinite)
                return fail(Diagnostic::Code::DivisionByZero, instruction.range);
            value.value = 1.0 / value.value;
            value.domain = value.domain == ValueDomain::Hertz
                ? ValueDomain::Seconds
                : value.domain == ValueDomain::Seconds
                    ? ValueDomain::Hertz
                    : ValueDomain::Scalar;
            return true;
        case detail::InstructionOp::Sign:
            value = {
                value.value > 0.0 ? 1.0 : value.value < 0.0 ? -1.0 : 0.0,
                ValueDomain::Scalar};
            return true;
        case detail::InstructionOp::Truncate:
            value.value = std::trunc(value.value);
            return true;
        case detail::InstructionOp::Round:
            value.value = std::round(value.value);
            return true;
        case detail::InstructionOp::ClipTo01:
            if (!requireScalar())
                return false;
            value.value = std::clamp(value.value, 0.0, 1.0);
            return true;
        case detail::InstructionOp::WrapTo02:
            if (!requireScalar())
                return false;
            value.value -= 2.0 * std::floor(value.value / 2.0);
            return true;
        case detail::InstructionOp::Cos:
        case detail::InstructionOp::Sin:
        case detail::InstructionOp::NormCos:
        case detail::InstructionOp::NormSin:
        {
            if (!requireScalar())
                return false;
            const auto normalized = instruction.op == detail::InstructionOp::NormCos
                || instruction.op == detail::InstructionOp::NormSin;
            const auto angle = normalized ? pi * value.value : value.value;
            value.value = instruction.op == detail::InstructionOp::Cos
                    || instruction.op == detail::InstructionOp::NormCos
                ? std::cos(angle)
                : std::sin(angle);
            return true;
        }
        case detail::InstructionOp::LogicValue:
            value = {value.value > 0.0 ? 1.0 : 0.0, ValueDomain::Scalar};
            return true;
        case detail::InstructionOp::RemoveUnits:
            value.domain = ValueDomain::Scalar;
            return true;
        case detail::InstructionOp::Floor:
            value.value = std::floor(value.value);
            return true;
        case detail::InstructionOp::Ceiling:
            value.value = std::ceil(value.value);
            return true;
        case detail::InstructionOp::RoundHalfUp:
            // Add 0.5 and truncate toward negative
            // infinity, so -2.5 rounded is -2.
            value.value = std::floor(value.value + 0.5);
            return true;
        case detail::InstructionOp::LinearUnitlessSOSPitch:
            // A number without units is a fraction
            // of 22050 hz, 11025.0 being 0.5; the pitch scale otherwise,
            // 22050 hz being 1 and each octave down 1/15 less.
            if (value.domain == ValueDomain::Scalar)
            {
                value.value /= 22050.0;
                return true;
            }
            if (!convertVal(value, ValueDomain::Hertz, instruction.range))
                return false;
            value = {1.0 + std::log2(std::max(value.value / 22050.0, std::exp2(-15.0))) / 15.0,
                ValueDomain::Scalar};
            return true;
        case detail::InstructionOp::ArcSin:
        case detail::InstructionOp::ArcCos:
            if (!requireScalar())
                return false;
            if (value.value < -1.0 || value.value > 1.0)
                return fail(Diagnostic::Code::InvalidArgument, instruction.range);
            value.value = instruction.op == detail::InstructionOp::ArcSin
                ? std::asin(value.value)
                : std::acos(value.value);
            return true;
        case detail::InstructionOp::ArcTan:
            if (!requireScalar())
                return false;
            value.value = std::atan(value.value);
            return true;
        case detail::InstructionOp::Tan:
            if (!requireScalar())
                return false;
            value.value = std::tan(value.value);
            return true;
        case detail::InstructionOp::Sinc:
            if (!requireScalar())
                return false;
            value.value = value.value == 0.0 ? 1.0 : std::sin(value.value) / value.value;
            return true;
        case detail::InstructionOp::ArrayIndex:
        {
            // A fractional index selects the element it falls in; the ends
            // hold outside the array.
            if (!std::isfinite(value.value) || instruction.number < 1.0)
                return fail(Diagnostic::Code::InvalidIndex, instruction.range);
            value = {
                std::clamp(std::trunc(value.value), 0.0, instruction.number - 1.0),
                ValueDomain::Scalar};
            return true;
        }
        default:
            return false;
        }
    }

    [[nodiscard]] bool executeBinaryFunction(
        const detail::Instruction& instruction) noexcept
    {
        ExpressionValue right;
        ExpressionValue left;
        if (!pop(right) || !pop(left))
            return false;
        if (instruction.op == detail::InstructionOp::RoundTo
            || instruction.op == detail::InstructionOp::TruncateTo
            || instruction.op == detail::InstructionOp::CeilingTo)
        {
            if (right.domain != ValueDomain::Scalar
                && !makeDomainsCompatible(left, right, instruction.range))
            {
                return false;
            }
            if (right.value == 0.0)
                return fail(Diagnostic::Code::DivisionByZero, instruction.range);
            const auto multiples = left.value / right.value;
            left.value = (instruction.op == detail::InstructionOp::RoundTo
                    ? std::round(multiples)
                    : instruction.op == detail::InstructionOp::TruncateTo
                        ? std::trunc(multiples)
                        : std::ceil(multiples))
                * right.value;
        }
        else if (instruction.op == detail::InstructionOp::LogBase
            || instruction.op == detail::InstructionOp::FloorLog)
        {
            // The logarithm of the receiver to the argument's base.
            if (left.domain != ValueDomain::Scalar
                || right.domain != ValueDomain::Scalar)
            {
                return fail(Diagnostic::Code::UnitMismatch, instruction.range);
            }
            if (left.value <= 0.0 || right.value <= 0.0 || right.value == 1.0)
                return fail(Diagnostic::Code::InvalidArgument, instruction.range);
            const auto logarithm = std::log(left.value) / std::log(right.value);
            left.value = instruction.op == detail::InstructionOp::FloorLog
                ? std::floor(logarithm + 1.0e-12)
                : logarithm;
        }
        else if (instruction.op == detail::InstructionOp::MaxAbs
            || instruction.op == detail::InstructionOp::MinAbs)
        {
            // Whichever has the greater, or lesser, magnitude, keeping its
            // sign.
            if (!makeDomainsCompatible(left, right, instruction.range))
                return false;
            const auto rightWins = instruction.op == detail::InstructionOp::MaxAbs
                ? std::abs(right.value) > std::abs(left.value)
                : std::abs(right.value) < std::abs(left.value);
            if (rightWins)
                left.value = right.value;
        }
        else
        {
            if (!makeDomainsCompatible(left, right, instruction.range))
                return false;
            left.value = instruction.op == detail::InstructionOp::Min
                ? std::min(left.value, right.value)
                : std::max(left.value, right.value);
        }
        ++instructionIndex_;
        return checked(left, instruction.range);
    }

    // Val converts between what a value measures: a pitch, a
    // frequency or a period.
    [[nodiscard]] bool convertVal(
        ExpressionValue& value,
        ValueDomain target,
        SourceRange range) noexcept
    {
        if (value.domain == ValueDomain::Samples
            && !normalizeTime(value, range))
        {
            return false;
        }
        if (value.domain == target)
            return true;
        if (value.domain == ValueDomain::NoteNumber)
        {
            value = {
                440.0 * std::pow(2.0, (value.value - 69.0) / 12.0),
                ValueDomain::Hertz};
        }
        else if (value.domain == ValueDomain::Seconds)
        {
            if (value.value == 0.0)
                return fail(Diagnostic::Code::DivisionByZero, range);
            value = {1.0 / value.value, ValueDomain::Hertz};
        }
        if (value.domain != ValueDomain::Hertz)
            return fail(Diagnostic::Code::UnitMismatch, range);
        if (target == ValueDomain::Hertz)
            return true;
        if (target == ValueDomain::Seconds)
        {
            if (value.value == 0.0)
                return fail(Diagnostic::Code::DivisionByZero, range);
            value = {1.0 / value.value, ValueDomain::Seconds};
            return true;
        }
        if (target == ValueDomain::NoteNumber)
        {
            if (value.value <= 0.0)
                return fail(Diagnostic::Code::InvalidArgument, range);
            value = {
                69.0 + 12.0 * std::log2(value.value / 440.0),
                ValueDomain::NoteNumber};
            return true;
        }
        return fail(Diagnostic::Code::UnitMismatch, range);
    }

    [[nodiscard]] bool executeClamp(
        const detail::Instruction& instruction) noexcept
    {
        ExpressionValue high;
        ExpressionValue low;
        ExpressionValue value;
        if (!pop(high) || !pop(low) || !pop(value)
            || !makeThreeDomainsCompatible(
                value,
                low,
                high,
                instruction.range))
        {
            return false;
        }
        if (low.value > high.value)
            return fail(Diagnostic::Code::InvalidArgument, instruction.range);
        value.value = std::clamp(value.value, low.value, high.value);
        ++instructionIndex_;
        return checked(value, instruction.range);
    }

    // Smalltalk's between: min and: max, 1 when the receiver lies in the
    // closed interval.
    [[nodiscard]] bool executeBetween(
        const detail::Instruction& instruction) noexcept
    {
        ExpressionValue high;
        ExpressionValue low;
        ExpressionValue value;
        if (!pop(high) || !pop(low) || !pop(value)
            || !makeThreeDomainsCompatible(
                value,
                low,
                high,
                instruction.range))
        {
            return false;
        }
        value = {
            value.value >= low.value && value.value <= high.value ? 1.0 : 0.0,
            ValueDomain::Scalar};
        ++instructionIndex_;
        return checked(value, instruction.range);
    }

    [[nodiscard]] bool executeMap(
        const detail::Instruction& instruction) noexcept
    {
        std::array<ExpressionValue, 5> arguments;
        for (std::size_t reverse = arguments.size(); reverse > 0; --reverse)
        {
            if (!pop(arguments[reverse - 1]))
                return false;
        }
        if (!makeThreeDomainsCompatible(
                arguments[0],
                arguments[1],
                arguments[2],
                instruction.range)
            || !makeDomainsCompatible(arguments[3], arguments[4], instruction.range))
        {
            return false;
        }
        const auto inputSpan = arguments[2].value - arguments[1].value;
        if (inputSpan == 0.0 || !std::isfinite(inputSpan))
            return fail(Diagnostic::Code::InvalidArgument, instruction.range);
        const auto amount =
            (arguments[0].value - arguments[1].value) / inputSpan;
        ExpressionValue result{
            arguments[3].value
                + (arguments[4].value - arguments[3].value) * amount,
            arguments[3].domain};
        ++instructionIndex_;
        return checked(result, instruction.range);
    }

    [[nodiscard]] bool executeArrayMap(
        const detail::Instruction& instruction) noexcept
    {
        const auto base = static_cast<std::size_t>(instruction.referenceIndex);
        const auto count = static_cast<std::size_t>(instruction.target);
        if (count < 2 || count > 15
            || base > storage_.localSlotCount
            || 2 + 2 * count > storage_.localSlotCount - base)
        {
            return fail(Diagnostic::Code::BudgetExceeded, instruction.range);
        }
        for (std::size_t index = 0; index < 2 + 2 * count; ++index)
        {
            if (!localAssigned_[base + index])
                return fail(Diagnostic::Code::BudgetExceeded, instruction.range);
        }
        const auto input = locals_[base].value;
        const auto interpolation = locals_[base + 1].value;
        if (!std::isfinite(input) || !std::isfinite(interpolation)
            || interpolation < 0.0 || interpolation > 1.0)
        {
            return fail(Diagnostic::Code::InvalidArgument, instruction.range);
        }
        const auto x = [&](std::size_t index) {
            return locals_[base + 2 + index].value;
        };
        const auto y = [&](std::size_t index) {
            return locals_[base + 2 + count + index].value;
        };
        for (std::size_t index = 0; index < count; ++index)
        {
            if (!std::isfinite(x(index)) || !std::isfinite(y(index))
                || (index > 0 && !(x(index) > x(index - 1))))
            {
                return fail(Diagnostic::Code::InvalidArgument, instruction.range);
            }
        }

        double value = y(0);
        if (input >= x(count - 1))
        {
            value = y(count - 1);
        }
        else if (input > x(0))
        {
            std::size_t left = 0;
            while (left + 1 < count - 1 && input >= x(left + 1))
                ++left;
            const auto width = x(left + 1) - x(left);
            const auto amount = (input - x(left)) / width;
            const auto linear = std::lerp(y(left), y(left + 1), amount);
            if (!std::isfinite(width) || width <= 0.0
                || !std::isfinite(amount) || !std::isfinite(linear))
            {
                return fail(Diagnostic::Code::NonFiniteResult, instruction.range);
            }
            if (interpolation <= 0.5)
            {
                value = std::lerp(
                    y(left), linear, interpolation * 2.0);
            }
            else
            {
                const auto slope = [&](std::size_t index) {
                    if (index == 0 || index + 1 == count)
                        return 0.0;
                    return (y(index + 1) - y(index - 1))
                        / (x(index + 1) - x(index - 1));
                };
                const auto leftSlope = slope(left);
                const auto rightSlope = slope(left + 1);
                const auto amount2 = amount * amount;
                const auto amount3 = amount2 * amount;
                const auto spline =
                    (2.0 * amount3 - 3.0 * amount2 + 1.0) * y(left)
                    + (amount3 - 2.0 * amount2 + amount)
                        * width * leftSlope
                    + (-2.0 * amount3 + 3.0 * amount2)
                        * y(left + 1)
                    + (amount3 - amount2) * width * rightSlope;
                if (!std::isfinite(leftSlope)
                    || !std::isfinite(rightSlope)
                    || !std::isfinite(spline))
                {
                    return fail(Diagnostic::Code::NonFiniteResult, instruction.range);
                }
                value = std::lerp(
                    linear, spline, (interpolation - 0.5) * 2.0);
            }
        }
        ++instructionIndex_;
        return checked({value, ValueDomain::Scalar}, instruction.range);
    }

    [[nodiscard]] bool executeListSelect(
        const detail::Instruction& instruction) noexcept
    {
        ExpressionValue indexValue;
        if (!pop(indexValue))
            return false;
        if (instruction.referenceIndex >= storage_.listDispatches.size())
        {
            return fail(
                Diagnostic::Code::BudgetExceeded,
                instruction.range);
        }
        const auto& dispatch =
            storage_.listDispatches[instruction.referenceIndex];
        if (dispatch.elementCount == 0
            || dispatch.elementCount
                > ExpressionLimits::maximumListElements)
        {
            return fail(
                Diagnostic::Code::BudgetExceeded,
                instruction.range);
        }
        if (indexValue.domain != ValueDomain::Scalar
            || !std::isfinite(indexValue.value)
            || indexValue.value < 0.0
            || std::trunc(indexValue.value) != indexValue.value
            || indexValue.value
                >= static_cast<double>(dispatch.elementCount))
        {
            return fail(
                Diagnostic::Code::InvalidIndex,
                instruction.range);
        }
        const auto index = static_cast<std::size_t>(indexValue.value);
        const auto target = dispatch.targets[index];
        if (target >= storage_.instructions.size())
        {
            return fail(
                Diagnostic::Code::BudgetExceeded,
                instruction.range);
        }
        instructionIndex_ = target;
        return true;
    }

    [[nodiscard]] bool executeSequenceIndexValidation(
        const detail::Instruction& instruction) noexcept
    {
        if (stackSize_ == 0)
            return fail(Diagnostic::Code::BudgetExceeded, instruction.range);
        const auto& index = stack_[stackSize_ - 1];
        if (index.domain != ValueDomain::Scalar
            || !std::isfinite(index.value)
            || index.value < 0.0
            || std::trunc(index.value) != index.value)
        {
            return fail(Diagnostic::Code::InvalidIndex, instruction.range);
        }
        ++instructionIndex_;
        return true;
    }

    [[nodiscard]] bool executeSequenceCountValidation(
        const detail::Instruction& instruction) noexcept
    {
        if (stackSize_ == 0)
            return fail(Diagnostic::Code::BudgetExceeded, instruction.range);
        const auto& count = stack_[stackSize_ - 1];
        if (count.domain != ValueDomain::Scalar
            || !std::isfinite(count.value)
            || count.value < 1.0
            || count.value
                > static_cast<double>(
                    ExpressionLimits::maximumSequenceElements)
            || std::trunc(count.value) != count.value)
        {
            return fail(
                Diagnostic::Code::InvalidSequenceCount,
                instruction.range);
        }
        ++instructionIndex_;
        return true;
    }

    [[nodiscard]] bool executeSequenceBoundsCheck(
        const detail::Instruction& instruction) noexcept
    {
        if (stackSize_ < 2)
            return fail(Diagnostic::Code::BudgetExceeded, instruction.range);
        const auto count = stack_[stackSize_ - 1];
        const auto index = stack_[stackSize_ - 2];
        if (index.value >= count.value)
            return fail(Diagnostic::Code::InvalidIndex, instruction.range);
        --stackSize_;
        ++instructionIndex_;
        return true;
    }

    [[nodiscard]] bool executeSeries(
        const detail::Instruction& instruction) noexcept
    {
        ExpressionValue step;
        ExpressionValue first;
        ExpressionValue index;
        if (!pop(step) || !pop(first) || !pop(index))
            return false;
        if (index.domain != ValueDomain::Scalar
            || !makeDomainsCompatible(first, step, instruction.range))
        {
            return false;
        }
        const ExpressionValue result{
            first.value + step.value * index.value,
            first.domain};
        ++instructionIndex_;
        return checked(result, instruction.range);
    }

    [[nodiscard]] bool executeStoreLocal(
        const detail::Instruction& instruction) noexcept
    {
        if (instruction.referenceIndex >= storage_.localSlotCount
            || instruction.referenceIndex >= locals_.size())
        {
            return fail(Diagnostic::Code::BudgetExceeded, instruction.range);
        }
        ExpressionValue value;
        if (!pop(value))
            return false;
        // A kept element keeps its domain.
        if (instruction.number > 0.0)
        {
            if (!std::isfinite(value.value))
                return fail(Diagnostic::Code::NonFiniteResult, instruction.range);
        }
        else if (!convertToExpected(value, instruction.domain, instruction.range))
            return false;
        locals_[instruction.referenceIndex] = value;
        localAssigned_[instruction.referenceIndex] = true;
        ++instructionIndex_;
        return true;
    }

    [[nodiscard]] bool executeLoadLocal(
        const detail::Instruction& instruction) noexcept
    {
        if (instruction.referenceIndex >= storage_.localSlotCount
            || instruction.referenceIndex >= locals_.size()
            || !localAssigned_[instruction.referenceIndex])
        {
            return fail(Diagnostic::Code::BudgetExceeded, instruction.range);
        }
        ++instructionIndex_;
        return push(locals_[instruction.referenceIndex]);
    }

    [[nodiscard]] bool executePickLocal(const detail::Instruction& instruction) noexcept
    {
        ExpressionValue index;
        if (!pop(index))
            return false;
        const auto count = static_cast<std::size_t>(instruction.target);
        if (index.domain != ValueDomain::Scalar || !std::isfinite(index.value) || index.value < 0.0
            || std::trunc(index.value) != index.value || index.value >= static_cast<double>(count))
            return fail(Diagnostic::Code::InvalidIndex, instruction.range);
        const auto slot = static_cast<std::size_t>(instruction.referenceIndex) + static_cast<std::size_t>(index.value);
        if (slot >= storage_.localSlotCount || slot >= locals_.size() || !localAssigned_[slot])
            return fail(Diagnostic::Code::BudgetExceeded, instruction.range);
        ++instructionIndex_;
        return push(locals_[slot]);
    }

    [[nodiscard]] bool executeValidateDomain(
        const detail::Instruction& instruction) noexcept
    {
        if (stackSize_ == 0)
            return fail(Diagnostic::Code::BudgetExceeded, instruction.range);
        if (!convertToExpected(
                stack_[stackSize_ - 1],
                instruction.domain,
                instruction.range))
        {
            return false;
        }
        ++instructionIndex_;
        return true;
    }

    [[nodiscard]] bool executeJumpIfFalse(
        const detail::Instruction& instruction) noexcept
    {
        ExpressionValue condition;
        if (!pop(condition))
            return false;
        if (condition.domain != ValueDomain::Scalar)
            return fail(Diagnostic::Code::UnitMismatch, instruction.range);
        if (instruction.target > storage_.instructions.size())
            return fail(Diagnostic::Code::BudgetExceeded, instruction.range);
        instructionIndex_ = condition.value == 0.0
            ? instruction.target
            : instructionIndex_ + 1;
        return true;
    }

    [[nodiscard]] bool executeJump(
        const detail::Instruction& instruction) noexcept
    {
        if (instruction.target > storage_.instructions.size())
            return fail(Diagnostic::Code::BudgetExceeded, instruction.range);
        instructionIndex_ = instruction.target;
        return true;
    }

    [[nodiscard]] bool convertToExpected(
        ExpressionValue& value,
        ValueDomain expectedDomain,
        SourceRange range) noexcept
    {
        if (value.domain == expectedDomain)
            return admissible(value.value)
                || fail(Diagnostic::Code::NonFiniteResult, range);
        if (value.domain == ValueDomain::NoteNumber
            && expectedDomain == ValueDomain::Hertz)
        {
            value.value = 440.0
                * std::pow(2.0, (value.value - 69.0) / 12.0);
            value.domain = ValueDomain::Hertz;
            return admissible(value.value)
                || fail(Diagnostic::Code::NonFiniteResult, range);
        }
        if (value.domain == ValueDomain::Samples
            && expectedDomain == ValueDomain::Seconds)
        {
            if (!normalizeTime(value, range))
                return false;
            return true;
        }
        if (usesValMessages(storage_.languageVersion)
            && expectedDomain != ValueDomain::Scalar)
        {
            // Val reads a plain number in the field's own unit and
            // converts a frequency, pitch or period to it.
            if (value.domain == ValueDomain::Scalar)
                value.domain = expectedDomain;
            else if (!convertVal(value, expectedDomain, range))
                return false;
            return admissible(value.value)
                || fail(Diagnostic::Code::NonFiniteResult, range);
        }
        // A key number in a field of plain
        // numbers, as !KeyNumber / 127 in a Sample's Start, is its number,
        // as a Constant's Value takes it.
        if (usesValMessages(storage_.languageVersion)
            && value.domain == ValueDomain::NoteNumber)
        {
            value.domain = ValueDomain::Scalar;
            return admissible(value.value)
                || fail(Diagnostic::Code::NonFiniteResult, range);
        }
        return fail(Diagnostic::Code::UnitMismatch, range);
    }

    const detail::ExpressionStorage& storage_;
    const EvaluationContext& context_;
    const ExpressionState* state_ = nullptr;
    bool pausesSkippedTime_ = false;
    ExpressionState ownedCandidateState_;
    ExpressionState* candidateState_ = nullptr;
    std::array<ExpressionValue,
               ExpressionLimits::maximumEvaluationStack> stack_{};
    std::array<ExpressionValue,
               ExpressionLimits::maximumDefinitionLocals> locals_{};
    std::array<bool,
               ExpressionLimits::maximumDefinitionLocals> localAssigned_{};
    std::size_t stackSize_ = 0;
    std::size_t instructionIndex_ = 0;
    ExecutionFailure failure_;
};

[[nodiscard]] std::string diagnosticMessage(
    const detail::ExpressionStorage& storage,
    const ExecutionFailure& failure)
{
    switch (failure.code)
    {
    case Diagnostic::Code::UnknownEvent:
    case Diagnostic::Code::UnknownVariable:
    case Diagnostic::Code::UnknownIdentifier:
        if (failure.instructionIndex < storage.instructions.size())
        {
            const auto& instruction =
                storage.instructions[failure.instructionIndex];
            const auto& references = failure.code
                    == Diagnostic::Code::UnknownEvent
                ? storage.eventReferences
                : failure.code == Diagnostic::Code::UnknownVariable
                    ? storage.variableReferences
                    : storage.identifiers;
            if (instruction.referenceIndex < references.size())
            {
                return std::string{
                           failure.code == Diagnostic::Code::UnknownEvent
                               ? "Unknown event reference: "
                               : failure.code == Diagnostic::Code::UnknownVariable
                                   ? "Unknown variable reference: "
                               : "Unknown identifier: "}
                    + references[instruction.referenceIndex];
            }
        }
        return failure.code == Diagnostic::Code::UnknownEvent
            ? "Unknown event reference"
            : failure.code == Diagnostic::Code::UnknownVariable
                ? "Unknown variable reference"
                : "Unknown identifier";
    case Diagnostic::Code::DivisionByZero:
        return "Division by zero";
    case Diagnostic::Code::UnitMismatch:
        return "Expression operands or result have incompatible value domains";
    case Diagnostic::Code::MissingSampleRate:
        return "A positive finite sample rate is required";
    case Diagnostic::Code::MissingTempo:
        return "A positive finite tempo is required";
    case Diagnostic::Code::MissingState:
        return "Expression requires matching prepared session state";
    case Diagnostic::Code::NonFiniteResult:
        return "Expression produced a non-finite value";
    case Diagnostic::Code::InvalidArgument:
        return "Pure expression function received an invalid argument";
    case Diagnostic::Code::InvalidIndex:
        if (failure.instructionIndex < storage.instructions.size())
        {
            const auto op = storage.instructions[failure.instructionIndex].op;
            if (op == detail::InstructionOp::ValidateSequenceIndex
                || op == detail::InstructionOp::CheckSequenceBounds)
            {
                return "Pure expression series index is invalid";
            }
        }
        return "Pure expression selection-list index is invalid";
    case Diagnostic::Code::InvalidSequenceCount:
        return "Pure expression series count is invalid";
    case Diagnostic::Code::InvalidDefinition:
        return "User-defined expression is invalid";
    case Diagnostic::Code::RecursiveDefinition:
        return "Recursive user-defined expressions are not allowed";
    case Diagnostic::Code::BudgetExceeded:
        return "Compiled expression violated its fixed execution budget";
    default:
        return "Expression evaluation failed";
    }
}
}

Expression::Expression(
    std::shared_ptr<const detail::ExpressionStorage> storage)
    : storage_(std::move(storage))
{
}

std::optional<Expression::SoundSplit> Expression::splitAroundSoundReferences(
    std::string_view source) const
{
    if (storage_ == nullptr || storage_->nodes.empty())
        return std::nullopt;
    const auto& nodes = storage_->nodes;
    struct Facts
    {
        bool sound = false;
        bool live = false;
        bool opaque = false;
    };
    std::vector<std::optional<Facts>> facts(nodes.size());
    const auto children = [&](const detail::ExpressionNode& node) {
        std::vector<NodeId> result;
        switch (node.kind)
        {
        case detail::NodeKind::Unary:
        case detail::NodeKind::UnitApplication:
            result.push_back(node.left);
            break;
        case detail::NodeKind::Binary:
        case detail::NodeKind::Comparison:
            result.push_back(node.left);
            result.push_back(node.right);
            break;
        case detail::NodeKind::Call:
        case detail::NodeKind::UserCall:
            for (std::size_t index = 0; index < node.argumentCount; ++index)
                result.push_back(node.arguments[index]);
            break;
        // `of:` and its kin: the index (and an array map's other operand)
        // and the list's elements, as `(Threshold2 L nextRandom * 6) of:
        // #(0 2 4 7 9 12)`.
        case detail::NodeKind::ArrayMap:
            result.push_back(node.right);
            [[fallthrough]];
        case detail::NodeKind::ListIndex:
            result.push_back(node.left);
            for (std::size_t element = 0; element < node.listElementCount; ++element)
            {
                const auto at = static_cast<std::size_t>(node.listElementOffset) + element;
                if (at < storage_->listElements.size())
                    result.push_back(storage_->listElements[at]);
            }
            break;
        default:
            break;
        }
        return result;
    };
    const std::function<Facts(NodeId)> examine = [&](NodeId id) -> Facts {
        if (id >= nodes.size())
            return {false, false, true};
        if (facts[id].has_value())
            return *facts[id];
        const auto& node = nodes[id];
        Facts result;
        switch (node.kind)
        {
        case detail::NodeKind::Literal:
            break;
        case detail::NodeKind::EventReference:
        case detail::NodeKind::VariableReference:
            result.live = true;
            break;
        case detail::NodeKind::IdentifierReference:
            if (parseSoundReference(node.name).has_value())
                result.sound = true;
            else
                result.live = true;
            break;
        case detail::NodeKind::Call:
        case detail::NodeKind::UserCall:
            result.live = true;
            [[fallthrough]];
        case detail::NodeKind::Unary:
        case detail::NodeKind::UnitApplication:
        case detail::NodeKind::Binary:
        case detail::NodeKind::Comparison:
        case detail::NodeKind::ListIndex:
        case detail::NodeKind::ArrayMap:
            for (const auto child : children(node))
            {
                const auto childFacts = examine(child);
                result.sound = result.sound || childFacts.sound;
                result.live = result.live || childFacts.live;
                result.opaque = result.opaque || childFacts.opaque;
            }
            break;
        default:
            result.opaque = true;
            break;
        }
        facts[id] = result;
        return result;
    };
    std::vector<NodeId> parts;
    bool splittable = true;
    const std::function<void(NodeId)> collect = [&](NodeId id) {
        const auto nodeFacts = examine(id);
        if (!nodeFacts.sound)
        {
            if (nodeFacts.live)
                parts.push_back(id);
            return;
        }
        const auto& node = nodes[id];
        // A function of a Sound is evaluated in the skeleton; it must not
        // keep state or follow time, which preparing the skeleton checks.
        if (nodeFacts.opaque || node.kind == detail::NodeKind::UserCall)
        {
            splittable = false;
            return;
        }
        for (const auto child : children(node))
            collect(child);
    };
    collect(storage_->root);
    if (!splittable)
        return std::nullopt;
    std::ranges::sort(parts, [&](NodeId left, NodeId right) {
        return nodes[left].range.offset < nodes[right].range.offset;
    });
    SoundSplit split;
    std::string skeleton(source);
    for (std::size_t index = parts.size(); index-- > 0;)
    {
        const auto range = nodes[parts[index]].range;
        if (range.offset > source.size() || range.length > source.size() - range.offset
            || (index + 1 < parts.size()
                && range.offset + range.length > nodes[parts[index + 1]].range.offset))
        {
            return std::nullopt;
        }
        skeleton.replace(range.offset, range.length, "__kc_live_" + std::to_string(index + 1));
    }
    for (const auto part : parts)
    {
        const auto range = nodes[part].range;
        split.liveParts.emplace_back(source.substr(range.offset, range.length));
    }
    split.skeleton = std::move(skeleton);
    return split;
}

NodeId Expression::root() const
{
    return storage_ != nullptr ? storage_->root : 0;
}

ExpressionLanguageVersion Expression::languageVersion() const
{
    return storage_ != nullptr
        ? storage_->languageVersion
        : ExpressionLanguageVersion::LegacyV1;
}

std::size_t Expression::instructionCount() const
{
    return storage_ != nullptr ? storage_->instructions.size() : 0;
}

std::size_t Expression::maximumStackDepth() const
{
    return storage_ != nullptr ? storage_->maximumStackDepth : 0;
}

const std::vector<std::string>& Expression::eventReferences() const
{
    static const std::vector<std::string> empty;
    return storage_ != nullptr ? storage_->eventReferences : empty;
}

const std::vector<std::string>& Expression::variableReferences() const
{
    static const std::vector<std::string> empty;
    return storage_ != nullptr ? storage_->variableReferences : empty;
}

const std::vector<VariableReferenceMetadata>&
Expression::variableReferenceMetadata() const
{
    static const std::vector<VariableReferenceMetadata> empty;
    return storage_ != nullptr
        ? storage_->variableReferenceMetadata
        : empty;
}

const std::vector<std::string>& Expression::identifiers() const
{
    static const std::vector<std::string> empty;
    return storage_ != nullptr ? storage_->identifiers : empty;
}

bool Expression::readsMidiTimingClock() const noexcept
{
    return storage_ != nullptr
        && std::ranges::any_of(storage_->instructions, [](const detail::Instruction& instruction) {
               return instruction.op == detail::InstructionOp::MidiTimingClock;
           });
}

std::size_t Expression::stateSlotCount() const
{
    return storage_ != nullptr ? storage_->stateSlotCount : 0;
}

bool Expression::isStateful() const
{
    return stateSlotCount() != 0;
}

bool Expression::usesTime() const noexcept
{
    return storage_ != nullptr && storage_->usesTime;
}

bool Expression::startsFromItsStartingValues() const noexcept
{
    return storage_ != nullptr && storage_->startsFromItsStartingValues;
}

bool Expression::mayLeaveBranchesUntaken() const noexcept
{
    return storage_ != nullptr
        && std::ranges::any_of(storage_->instructions, [](const detail::Instruction& instruction) {
               return instruction.op == detail::InstructionOp::JumpIfFalse
                   || instruction.op == detail::InstructionOp::Jump;
           });
}

bool Expression::usesVoiceNumber() const noexcept
{
    return storage_ != nullptr && storage_->voiceNumberInstruction.has_value();
}

namespace
{
// Replaces every reference to one identifier with a literal, in the syntax
// tree and the compiled program; null when the identifier is not used.
std::shared_ptr<detail::ExpressionStorage> bindIdentifierLiteral(
    const detail::ExpressionStorage& storage,
    std::string_view name,
    double number,
    Unit unit)
{
    const auto marker = std::ranges::find(storage.identifiers, name);
    if (marker == storage.identifiers.end())
        return nullptr;
    const auto markerIndex = static_cast<std::size_t>(marker - storage.identifiers.begin());
    auto specialized = std::make_shared<detail::ExpressionStorage>(storage);
    specialized->identifiers.erase(
        specialized->identifiers.begin()
        + static_cast<std::ptrdiff_t>(markerIndex));
    for (auto& node : specialized->nodes)
    {
        if (node.kind != detail::NodeKind::IdentifierReference)
            continue;
        if (node.name == name)
        {
            node.kind = detail::NodeKind::Literal;
            node.name.clear();
            node.number = number;
            node.unit = unit;
            node.referenceIndex = 0;
        }
    }
    for (auto& instruction : specialized->instructions)
    {
        if (instruction.op != detail::InstructionOp::ResolveIdentifier)
            continue;
        if (instruction.referenceIndex == markerIndex)
        {
            instruction.op = detail::InstructionOp::PushLiteral;
            instruction.number = number;
            instruction.unit = unit;
            instruction.referenceIndex = 0;
        }
        else if (instruction.referenceIndex > markerIndex)
            --instruction.referenceIndex;
    }
    return specialized;
}

Unit literalUnit(ValueDomain domain)
{
    switch (domain)
    {
    case ValueDomain::Scalar:
        return Unit::None;
    case ValueDomain::Hertz:
        return Unit::Hertz;
    case ValueDomain::Seconds:
        return Unit::Seconds;
    case ValueDomain::NoteNumber:
        return Unit::NoteNumber;
    case ValueDomain::Samples:
        return Unit::Samples;
    }
    return Unit::None;
}
}

std::optional<Expression> Expression::specializeVoiceNumber(
    std::uint16_t oneBasedVoiceNumber) const
{
    if (storage_ == nullptr || oneBasedVoiceNumber == 0)
        return std::nullopt;
    if (!usesVoiceNumber())
        return *this;
    auto specialized = bindIdentifierLiteral(
        *storage_, voiceNumberIdentifier, oneBasedVoiceNumber, Unit::None);
    if (specialized == nullptr)
        return std::nullopt;
    specialized->voiceNumberInstruction.reset();
    return Expression{std::move(specialized)};
}

std::optional<Expression> Expression::specializeIdentifier(
    std::string_view name,
    ExpressionValue value) const
{
    if (storage_ == nullptr || name == voiceNumberIdentifier)
        return std::nullopt;
    auto specialized = bindIdentifierLiteral(
        *storage_, name, value.value, literalUnit(value.domain));
    if (specialized == nullptr)
        return *this;
    return Expression{std::move(specialized)};
}

bool Expression::structurallyEquivalent(
    const Expression& other) const noexcept
{
    if (storage_ == nullptr || other.storage_ == nullptr)
        return storage_ == other.storage_;
    return storage_->languageVersion
            == other.storage_->languageVersion
        && storage_->root == other.storage_->root
        && storage_->nodes == other.storage_->nodes
        && storage_->listElements
            == other.storage_->listElements
        && storage_->eventReferences
            == other.storage_->eventReferences
        && storage_->variableReferences
            == other.storage_->variableReferences
        && storage_->variableReferenceMetadata
            == other.storage_->variableReferenceMetadata
        && storage_->identifiers == other.storage_->identifiers
        && storage_->instructions
            == other.storage_->instructions
        && storage_->listDispatches
            == other.storage_->listDispatches
        && storage_->maximumStackDepth
            == other.storage_->maximumStackDepth
        && storage_->localSlotCount
            == other.storage_->localSlotCount
        && storage_->stateSlotCount
            == other.storage_->stateSlotCount
        && storage_->usesTime == other.storage_->usesTime
        && storage_->voiceNumberInstruction == other.storage_->voiceNumberInstruction;
}

bool Expression::computesSameAs(const Expression& other) const noexcept
{
    if (storage_ == nullptr || other.storage_ == nullptr
        || storage_->instructions.empty()
        || other.storage_->instructions.empty())
    {
        return false;
    }
    const auto sameInstruction = [](const detail::Instruction& left,
                                    const detail::Instruction& right) {
        return left.op == right.op
            && std::bit_cast<std::uint64_t>(left.number)
                == std::bit_cast<std::uint64_t>(right.number)
            && left.unit == right.unit
            && left.referenceIndex == right.referenceIndex
            && left.target == right.target
            && left.domain == right.domain;
    };
    return std::ranges::equal(
               storage_->instructions,
               other.storage_->instructions,
               sameInstruction)
        && storage_->eventReferences == other.storage_->eventReferences
        && storage_->variableReferences == other.storage_->variableReferences
        && storage_->identifiers == other.storage_->identifiers
        && storage_->listDispatches == other.storage_->listDispatches
        && storage_->localSlotCount == other.storage_->localSlotCount
        && storage_->stateSlotCount == other.storage_->stateSlotCount
        && storage_->usesTime == other.storage_->usesTime;
}

bool Expression::sharesCompiledStorageWith(
    const Expression& other) const noexcept
{
    return storage_ == other.storage_;
}

std::optional<std::vector<std::string>> scanEventReferenceNames(
    std::string_view source,
    ExpressionLanguageVersion languageVersion)
{
    if (source.size() > ExpressionLimits::maximumSourceBytes)
        return std::nullopt;
    std::vector<std::string> names;
    Lexer lexer(source, languageVersion);
    while (true)
    {
        const auto token = lexer.next();
        if (token.kind == TokenKind::End)
            return names;
        if (token.kind != TokenKind::Event
            || std::ranges::find(names, token.text) != names.end())
            continue;
        if (names.size() >= ExpressionLimits::maximumEventReferences)
            return std::nullopt;
        names.emplace_back(token.text);
    }
}

std::optional<std::string> eventSourceEventName(std::string_view source)
{
    if (source.empty() || !isIdentifierStart(source.front())
        || !std::ranges::all_of(source, isIdentifierContinue))
        return std::nullopt;
    const auto name = eventSourceTokenName(source);
    if (!name.has_value())
        return std::nullopt;
    return std::string{*name};
}

std::optional<std::string> replaceEventReferences(
    std::string_view source,
    ExpressionLanguageVersion languageVersion,
    std::span<const EventReferenceReplacement> replacements)
{
    if (source.size() > ExpressionLimits::maximumSourceBytes)
        return std::nullopt;
    std::string rewritten;
    Lexer lexer(source, languageVersion);
    std::size_t copiedThrough = 0;
    auto changed = false;
    while (true)
    {
        const auto token = lexer.next();
        if (token.kind == TokenKind::End)
            break;
        if (!isEventValueToken(source, token))
            continue;
        const auto replacement = std::ranges::find(
            replacements, token.text, &EventReferenceReplacement::from);
        if (replacement == replacements.end())
            continue;
        rewritten.append(source.substr(
            copiedThrough, token.range.offset - copiedThrough));
        rewritten.push_back('(');
        rewritten.append(replacement->to);
        rewritten.push_back(')');
        copiedThrough = token.range.offset + token.range.length;
        changed = true;
        if (rewritten.size() > ExpressionLimits::maximumSourceBytes)
            return std::nullopt;
    }
    if (!changed)
        return std::nullopt;
    rewritten.append(source.substr(copiedThrough));
    if (rewritten.size() > ExpressionLimits::maximumSourceBytes)
        return std::nullopt;
    return rewritten;
}

std::optional<std::string> renameEventReferences(
    std::string_view source,
    ExpressionLanguageVersion languageVersion,
    std::span<const EventReferenceRename> renames)
{
    if (source.size() > ExpressionLimits::maximumSourceBytes)
        return std::nullopt;
    const auto validName = [](std::string_view name) {
        return !name.empty()
            && name.size() <= ExpressionLimits::maximumSourceBytes - 1
            && isIdentifierStart(name.front())
            && std::ranges::all_of(name.substr(1), isIdentifierContinue);
    };
    for (std::size_t index = 0; index < renames.size(); ++index)
    {
        if (!validName(renames[index].from)
            || !validName(renames[index].to))
            return std::nullopt;
        for (std::size_t earlier = 0; earlier < index; ++earlier)
            if (renames[earlier].from == renames[index].from)
                return std::nullopt;
    }

    std::string rewritten;
    rewritten.reserve(source.size());
    Lexer lexer(source, languageVersion);
    std::size_t copiedThrough = 0;
    while (true)
    {
        const auto token = lexer.next();
        if (token.kind == TokenKind::End)
            break;
        if (!isEventValueToken(source, token))
            continue;
        const auto replacement = std::ranges::find(
            renames, token.text, &EventReferenceRename::from);
        if (replacement == renames.end()
            || replacement->to == token.text)
            continue;
        const auto unchangedBytes = token.range.offset - copiedThrough;
        if (rewritten.size() >= ExpressionLimits::maximumSourceBytes
                - unchangedBytes
            || replacement->to.size()
                > ExpressionLimits::maximumSourceBytes
                    - rewritten.size() - unchangedBytes - 1)
            return std::nullopt;
        rewritten.append(source.substr(
            copiedThrough, token.range.offset - copiedThrough));
        rewritten.push_back('!');
        rewritten.append(replacement->to);
        copiedThrough = token.range.offset + token.range.length;
    }
    if (rewritten.size() > ExpressionLimits::maximumSourceBytes
            - (source.size() - copiedThrough))
        return std::nullopt;
    rewritten.append(source.substr(copiedThrough));
    return rewritten;
}

std::optional<std::string> withKeyPitchBends(
    std::string_view source,
    ExpressionLanguageVersion languageVersion)
{
    if (source.size() > ExpressionLimits::maximumSourceBytes)
        return std::nullopt;
    std::string rewritten;
    Lexer lexer(source, languageVersion);
    std::size_t copiedThrough = 0;
    while (true)
    {
        const auto token = lexer.next();
        if (token.kind == TokenKind::End)
            break;
        if (!isEventValueToken(source, token)
            || (token.text != "KeyPitch" && token.text != "KeyPitch12"))
            continue;
        rewritten.append(source.substr(
            copiedThrough, token.range.offset - copiedThrough));
        rewritten.append(token.text == "KeyPitch"
                ? "(!KeyPitch + !PitchBend nn)"
                : "(!KeyPitch + (!PitchBend * 12) nn)");
        copiedThrough = token.range.offset + token.range.length;
    }
    if (copiedThrough == 0)
        return std::nullopt;
    rewritten.append(source.substr(copiedThrough));
    if (rewritten.size() > ExpressionLimits::maximumSourceBytes)
        return std::nullopt;
    return rewritten;
}

std::optional<std::string> withRandomEventSources(
    std::string_view source,
    ExpressionLanguageVersion languageVersion)
{
    if (source.size() > ExpressionLimits::maximumSourceBytes)
        return std::nullopt;
    std::string rewritten;
    Lexer lexer(source, languageVersion);
    std::size_t copiedThrough = 0;
    auto changed = false;
    while (true)
    {
        const auto token = lexer.next();
        if (token.kind == TokenKind::End)
            break;
        if (!isEventValueToken(source, token)
            || (token.text != "Random" && token.text != "Random1000"))
            continue;
        rewritten.append(source.substr(
            copiedThrough, token.range.offset - copiedThrough));
        rewritten.append("(1 ms random)");
        copiedThrough = token.range.offset + token.range.length;
        changed = true;
    }
    if (!changed)
        return std::nullopt;
    rewritten.append(source.substr(copiedThrough));
    if (rewritten.size() > ExpressionLimits::maximumSourceBytes)
        return std::nullopt;
    return rewritten;
}

static std::optional<std::string> specializeReplicationVariablesImpl(
    std::string_view source,
    std::uint16_t oneBasedVoiceNumber,
    std::uint16_t numberVoices,
    bool preserveSourceOffsets)
{
    if (source.size() > ExpressionLimits::maximumSourceBytes
        || oneBasedVoiceNumber == 0
        || numberVoices == 0
        || oneBasedVoiceNumber > numberVoices)
        return std::nullopt;
    const auto number = std::to_string(oneBasedVoiceNumber);
    const auto total = std::to_string(numberVoices);
    std::string rewritten;
    rewritten.reserve(source.size());
    Lexer lexer(source, ExpressionLanguageVersion::VoiceV1);
    std::size_t copiedThrough = 0;
    while (true)
    {
        const auto token = lexer.next();
        if (token.kind == TokenKind::End)
            break;
        const std::string* replacement = nullptr;
        if (token.text == "?VoiceNumber")
            replacement = &number;
        else if (token.text == "?NumberVoices")
            replacement = &total;
        if (replacement == nullptr)
            continue;
        const auto unchangedBytes = token.range.offset - copiedThrough;
        const auto replacementBytes = preserveSourceOffsets
            ? token.range.length : replacement->size();
        if (rewritten.size() > ExpressionLimits::maximumSourceBytes
                - unchangedBytes
            || replacementBytes > ExpressionLimits::maximumSourceBytes
                - rewritten.size() - unchangedBytes)
            return std::nullopt;
        // A number glued to a neighbouring name or number would lex as a
        // different token (2?VoiceNumber -> 23, !Fader?VoiceNumber ->
        // !Fader3); keep it separate so such text fails to parse instead.
        const auto gluesTo = [&](std::size_t index) {
            const auto character = static_cast<unsigned char>(source[index]);
            return std::isalnum(character) != 0 || character == '_'
                || character == '.' || character == '!' || character == '?'
                || character == '@';
        };
        const auto leadingSpace =
            token.range.offset > 0 && gluesTo(token.range.offset - 1);
        const auto tokenEnd = token.range.offset + token.range.length;
        const auto trailingSpace =
            tokenEnd < source.size() && gluesTo(tokenEnd);
        const auto spacedBytes = replacement->size()
            + static_cast<std::size_t>(leadingSpace)
            + static_cast<std::size_t>(trailingSpace);
        if (!preserveSourceOffsets
            && spacedBytes > ExpressionLimits::maximumSourceBytes
                - rewritten.size() - unchangedBytes)
            return std::nullopt;
        rewritten.append(source.substr(copiedThrough, unchangedBytes));
        if (leadingSpace)
            rewritten.push_back(' ');
        rewritten.append(*replacement);
        if (preserveSourceOffsets)
        {
            rewritten.append(
                token.range.length - replacement->size()
                    - static_cast<std::size_t>(leadingSpace),
                ' ');
        }
        else if (trailingSpace)
        {
            rewritten.push_back(' ');
        }
        copiedThrough = tokenEnd;
    }
    if (rewritten.size() > ExpressionLimits::maximumSourceBytes
            - (source.size() - copiedThrough))
        return std::nullopt;
    rewritten.append(source.substr(copiedThrough));
    return rewritten;
}

std::optional<std::string> specializeReplicationVariables(
    std::string_view source,
    std::uint16_t oneBasedVoiceNumber,
    std::uint16_t numberVoices)
{
    return specializeReplicationVariablesImpl(
        source, oneBasedVoiceNumber, numberVoices, false);
}

std::optional<std::string> specializeReplicationVariablesForAudit(
    std::string_view source,
    std::uint16_t oneBasedVoiceNumber,
    std::uint16_t numberVoices)
{
    return specializeReplicationVariablesImpl(
        source, oneBasedVoiceNumber, numberVoices, true);
}

std::string formatSoundReference(const SoundReference& reference)
{
    auto name = reference.sound + " " + reference.channel;
    if (reference.periodMilliseconds.has_value())
    {
        std::array<char, 32> digits{};
        const auto [end, error] = std::to_chars(
            digits.data(), digits.data() + digits.size(), *reference.periodMilliseconds);
        if (error == std::errc{})
            name += ": " + std::string(digits.data(), end);
    }
    return name;
}

std::optional<SoundReference> parseSoundReference(std::string_view identifier)
{
    const auto space = identifier.find(' ');
    if (space == std::string_view::npos || space == 0 || space + 1 >= identifier.size())
        return std::nullopt;
    SoundReference reference;
    reference.sound = std::string(identifier.substr(0, space));
    const auto channel = identifier[space + 1];
    if (channel != 'L' && channel != 'R' && channel != 'M')
        return std::nullopt;
    reference.channel = channel;
    const auto rest = identifier.substr(space + 2);
    if (rest.empty())
        return reference;
    if (rest.size() < 3 || rest.substr(0, 2) != ": ")
        return std::nullopt;
    double period = 0.0;
    const auto digits = rest.substr(2);
    const auto [end, error] = talas::fromChars(digits.data(), digits.data() + digits.size(), period);
    if (error != std::errc{} || end != digits.data() + digits.size()
        || !std::isfinite(period) || period <= 0.0)
    {
        return std::nullopt;
    }
    reference.periodMilliseconds = period;
    return reference;
}

ParseResult parse(std::string_view source)
{
    return parse(source, ExpressionLanguageVersion::LegacyV1);
}

ParseResult parse(
    std::string_view source,
    ExpressionLanguageVersion languageVersion)
{
    static const ExpressionDefinitionLibrary emptyDefinitions;
    return parse(source, languageVersion, emptyDefinitions);
}

namespace
{
// A Val field may hold a Smalltalk program:
// temporaries between bars, statements ending in periods, assignments with
// :=, the last statement giving the field's value. The program
// becomes one expression, each temporary read as the value last assigned
// to it. Nothing for a field without temporaries or assignments; an error
// for a temporary not declared, read before it is assigned, or a statement
// before the last that assigns nothing.
struct FieldProgram
{
    std::optional<std::string> expression;
    std::string error;
};

[[nodiscard]] std::string_view trimmedField(std::string_view text) noexcept
{
    while (!text.empty() && isAsciiWhitespace(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && isAsciiWhitespace(text.back()))
        text.remove_suffix(1);
    return text;
}

// !Amp suffix2: (3), in an element a braced
// collect: program writes out, is the Event Value named with the number
// after it in two digits, !Amp03; suffix: (3) puts the number after it as
// it is, !Amp3.
[[nodiscard]] std::string withSuffixedNames(std::string element)
{
    const auto isName = [](char character) {
        return std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_';
    };
    for (std::size_t bang = element.find('!'); bang != std::string::npos; bang = element.find('!', bang + 1))
    {
        auto at = bang + 1;
        while (at < element.size() && isName(element[at]))
            ++at;
        const auto nameEnd = at;
        while (at < element.size() && isAsciiWhitespace(element[at]))
            ++at;
        const auto padded = element.compare(at, 8, "suffix2:") == 0;
        if (!padded && element.compare(at, 7, "suffix:") != 0)
            continue;
        at += padded ? 8 : 7;
        while (at < element.size() && isAsciiWhitespace(element[at]))
            ++at;
        if (at >= element.size() || element[at] != '(')
            continue;
        const auto close = element.find(')', at);
        if (close == std::string::npos)
            continue;
        long number = 0;
        const auto [end, error] = std::from_chars(element.data() + at + 1, element.data() + close, number);
        if (error != std::errc{} || end != element.data() + close || number < 0)
            continue;
        auto digits = std::to_string(number);
        if (padded && digits.size() < 2)
            digits.insert(digits.begin(), '0');
        element.replace(nameEnd, close + 1 - nameEnd, digits);
    }
    return element;
}

// An Interval collected through a one-argument
// block within braces, as {(0 to: 4) collect: [:i | ... i ...]}, is the
// Array of the block's value at each step, written out as #({... 0 ...}
// ... {... 4 ...}) before the expression parses; nothing when the source
// holds none.
[[nodiscard]] std::optional<std::string> withCollectedArrays(std::string_view source)
{
    const auto isName = [](char character) {
        return std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_';
    };
    std::string result;
    std::size_t copied = 0;
    auto changed = false;
    for (auto open = source.find('{'); open != std::string_view::npos; open = source.find('{', open + 1))
    {
        std::size_t at = open + 1;
        const auto skip = [&] {
            while (at < source.size() && isAsciiWhitespace(source[at]))
                ++at;
        };
        const auto expect = [&](std::string_view text) {
            skip();
            if (source.substr(at, text.size()) != text)
                return false;
            at += text.size();
            return true;
        };
        const auto integer = [&]() -> std::optional<long> {
            skip();
            const auto start = at;
            if (at < source.size() && source[at] == '-')
                ++at;
            while (at < source.size() && std::isdigit(static_cast<unsigned char>(source[at])) != 0)
                ++at;
            long value = 0;
            const auto [end, error] = std::from_chars(source.data() + start, source.data() + at, value);
            if (error != std::errc{} || end != source.data() + at || at == start)
                return std::nullopt;
            return value;
        };
        if (!expect("("))
            continue;
        const auto first = integer();
        if (!first.has_value() || !expect("to:"))
            continue;
        const auto last = integer();
        long step = 1;
        if (!last.has_value())
            continue;
        skip();
        if (source.substr(at, 3) == "by:")
        {
            at += 3;
            const auto by = integer();
            if (!by.has_value() || *by == 0)
                continue;
            step = *by;
        }
        if (!expect(")") || !expect("collect:") || !expect("[") || !expect(":"))
            continue;
        skip();
        const auto nameStart = at;
        while (at < source.size() && isName(source[at]))
            ++at;
        const auto name = source.substr(nameStart, at - nameStart);
        if (name.empty() || !expect("|"))
            continue;
        const auto bodyStart = at;
        auto depth = 0;
        while (at < source.size() && (source[at] != ']' || depth > 0))
        {
            depth += source[at] == '[' ? 1 : source[at] == ']' ? -1 : 0;
            ++at;
        }
        if (at >= source.size())
            continue;
        const auto body = source.substr(bodyStart, at - bodyStart);
        ++at;
        if (!expect("}"))
            continue;
        const auto count = (*last - *first) / step + 1;
        if (count < 1 || count > static_cast<long>(ExpressionLimits::maximumListElements))
            continue;
        std::string array = "#";
        array += eagerArrayMark;
        array += '(';
        for (long index = 0; index < count; ++index)
        {
            const auto value = std::to_string(*first + index * step);
            std::string element;
            for (std::size_t offset = 0; offset < body.size();)
            {
                const auto before = offset == 0 ? '\0' : body[offset - 1];
                const auto after = offset + name.size() < body.size() ? body[offset + name.size()] : '\0';
                if (body.substr(offset, name.size()) == name && !isName(before) && before != '!' && before != '?'
                    && !isName(after) && after != ':')
                {
                    element += "(" + value + ")";
                    offset += name.size();
                    continue;
                }
                element += body[offset++];
            }
            array += (index == 0 ? "{" : " {") + withSuffixedNames(element) + "}";
        }
        array += ")";
        result.append(source.substr(copied, open - copied));
        result += array;
        copied = at;
        open = at - 1;
        changed = true;
    }
    if (!changed)
        return std::nullopt;
    result.append(source.substr(copied));
    return result;
}

[[nodiscard]] FieldProgram inlineFieldProgram(std::string_view source)
{
    // Comments become spaces, so that their periods end nothing.
    std::string text(source);
    for (std::size_t at = 0; at < text.size(); ++at)
    {
        if (text[at] == '\'')
        {
            const auto close = text.find('\'', at + 1);
            if (close == std::string::npos)
                return {};
            at = close;
        }
        else if (text[at] == '"')
        {
            const auto close = text.find('"', at + 1);
            if (close == std::string::npos)
                return {};
            std::fill(text.begin() + static_cast<std::ptrdiff_t>(at), text.begin() + static_cast<std::ptrdiff_t>(close) + 1, ' ');
            at = close;
        }
    }
    const auto isNameStart = [](char character) {
        return std::isalpha(static_cast<unsigned char>(character)) || character == '_';
    };
    const auto isNameCharacter = [](char character) {
        return std::isalnum(static_cast<unsigned char>(character)) || character == '_';
    };
    std::size_t at = 0;
    while (at < text.size() && isAsciiWhitespace(text[at]))
        ++at;
    std::vector<std::string> temporaries;
    if (at < text.size() && text[at] == '|')
    {
        const auto close = text.find('|', at + 1);
        if (close == std::string::npos)
            return {std::nullopt, "A field's temporaries end with a bar"};
        std::size_t name = at + 1;
        while (name < close)
        {
            while (name < close && isAsciiWhitespace(text[name]))
                ++name;
            if (name == close)
                break;
            if (!isNameStart(text[name]))
                return {std::nullopt, "A field's temporaries are names between bars"};
            auto end = name;
            while (end < close && isNameCharacter(text[end]))
                ++end;
            temporaries.emplace_back(text.substr(name, end - name));
            name = end;
        }
        at = close + 1;
    }
    else if (text.find(":=") == std::string::npos)
        return {};
    // The statements, split at periods outside brackets and strings; a
    // period between digits is a decimal point.
    std::vector<std::string> statements;
    std::size_t depth = 0;
    std::size_t start = at;
    for (auto index = at; index <= text.size(); ++index)
    {
        if (index < text.size())
        {
            const auto character = text[index];
            if (character == '\'')
            {
                index = text.find('\'', index + 1);
                continue;
            }
            if (character == '(' || character == '[' || character == '{')
            {
                ++depth;
                continue;
            }
            if ((character == ')' || character == ']' || character == '}') && depth > 0)
            {
                --depth;
                continue;
            }
            const auto decimal = index > 0 && index + 1 < text.size()
                && std::isdigit(static_cast<unsigned char>(text[index + 1]))
                && (std::isdigit(static_cast<unsigned char>(text[index - 1]))
                    || std::isupper(static_cast<unsigned char>(text[index - 1])));
            if (character != '.' || depth > 0 || decimal)
                continue;
        }
        std::string statement(trimmedField(std::string_view{text}.substr(start, index - start)));
        if (!statement.empty())
            statements.push_back(std::move(statement));
        start = index + 1;
    }
    if (statements.empty())
        return {std::nullopt, "A field's program needs a statement"};
    // A temporary holding EventVariable new, with an initialValue: or 0,
    // is that EventVariable wherever it is read; in such
    // a program the other temporaries share their expressions.
    const auto eventVariableInitial = [&](std::string_view value) -> std::optional<std::string> {
        constexpr std::string_view created = "EventVariable";
        if (!value.starts_with(created))
            return std::nullopt;
        value = trimmedField(value.substr(created.size()));
        if (!value.starts_with("new") || (value.size() > 3 && isNameCharacter(value[3])))
            return std::nullopt;
        value = trimmedField(value.substr(3));
        if (value.empty())
            return std::string{"0"};
        constexpr std::string_view initial = "initialValue:";
        if (!value.starts_with(initial) || trimmedField(value.substr(initial.size())).empty())
            return std::nullopt;
        return std::string{trimmedField(value.substr(initial.size()))};
    };
    const auto withEventVariables = std::ranges::any_of(statements, [&](const std::string& statement) {
        const auto assign = statement.find(":=");
        return assign != std::string::npos
            && eventVariableInitial(trimmedField(std::string_view{statement}.substr(assign + 2))).has_value();
    });
    std::size_t eventVariables = 0;
    std::size_t sharedExpressions = 0;
    std::string everyMillisecond;
    std::vector<std::pair<std::string, std::string>> values;
    // The statement with each temporary read as its value.
    const auto substituted = [&](const std::string& statement, std::string& read) {
        read.clear();
        for (std::size_t index = 0; index < statement.size();)
        {
            const auto character = statement[index];
            if (character == '\'')
            {
                const auto close = statement.find('\'', index + 1);
                const auto end = close == std::string::npos ? statement.size() : close + 1;
                read.append(statement, index, end - index);
                index = end;
                continue;
            }
            if (!isNameStart(character))
            {
                read += character;
                ++index;
                continue;
            }
            auto end = index;
            while (end < statement.size() && isNameCharacter(statement[end]))
                ++end;
            const auto name = statement.substr(index, end - index);
            const auto marked = index > 0
                && (statement[index - 1] == '!' || statement[index - 1] == '?' || statement[index - 1] == '@'
                    || statement[index - 1] == '#' || isNameCharacter(statement[index - 1]));
            const auto keyword = end < statement.size() && statement[end] == ':'
                && (end + 1 >= statement.size() || statement[end + 1] != '=');
            const auto declared = std::ranges::find(temporaries, name) != temporaries.end();
            if (marked || keyword || !declared)
            {
                read += name;
                index = end;
                continue;
            }
            const auto value = std::ranges::find(values, name, &std::pair<std::string, std::string>::first);
            if (value == values.end())
                return "Temporary " + name + " is read before it is assigned";
            read += "(" + value->second + ")";
            index = end;
        }
        return std::string{};
    };
    std::string last;
    for (std::size_t index = 0; index < statements.size(); ++index)
    {
        const auto& statement = statements[index];
        std::size_t nameEnd = 0;
        while (nameEnd < statement.size() && isNameCharacter(statement[nameEnd]))
            ++nameEnd;
        auto assign = nameEnd;
        while (assign < statement.size() && isAsciiWhitespace(statement[assign]))
            ++assign;
        const auto assigns = nameEnd > 0 && isNameStart(statement[0])
            && statement.compare(assign, 2, ":=") == 0;
        std::string read;
        const auto failure = substituted(assigns ? statement.substr(assign + 2) : statement, read);
        if (!failure.empty())
            return {std::nullopt, failure};
        read = std::string(trimmedField(read));
        if (read.empty())
            return {std::nullopt, "A field's statement needs a value"};
        if (assigns && withEventVariables)
        {
            if (const auto initial = eventVariableInitial(read))
                read = "talasEventVariable(" + std::to_string(eventVariables++) + ", (" + *initial + "))";
            else
                read = "talasShared(" + std::to_string(sharedExpressions++) + ", (" + read + "))";
        }
        // Pos readEveryMillisecond, a statement of its own, has the
        // program evaluated every millisecond.
        constexpr std::string_view everyMillisecondMessage = "readEveryMillisecond";
        if (!assigns && index + 1 < statements.size() && withEventVariables && read.starts_with("(talasEventVariable(")
            && read.ends_with(everyMillisecondMessage))
        {
            everyMillisecond += "(" + read + "), ";
            continue;
        }
        if (assigns)
        {
            const auto name = statement.substr(0, nameEnd);
            if (std::ranges::find(temporaries, name) == temporaries.end())
                return {std::nullopt, "Temporary " + name + " is not declared between bars"};
            const auto value = std::ranges::find(values, name, &std::pair<std::string, std::string>::first);
            if (value == values.end())
                values.emplace_back(name, read);
            else
                value->second = read;
        }
        else if (index + 1 < statements.size())
            return {std::nullopt, "Only a field's last statement may give a value without assigning it"};
        last = std::move(read);
    }
    if (!everyMillisecond.empty())
        last = everyMillisecond + "(" + last + ")";
    return {std::move(last), {}};
}
}

FieldProgramExpression fieldProgramExpression(std::string_view source)
{
    auto inlined = inlineFieldProgram(source);
    return {std::move(inlined.expression), std::move(inlined.error)};
}

std::string withLeadingZeros(std::string_view source)
{
    // A field's program keeps its periods, which may end its statements.
    if (const auto program = inlineFieldProgram(source);
        program.expression.has_value() || !program.error.empty())
    {
        return std::string{source};
    }
    std::string result;
    std::size_t copied = 0;
    std::size_t at = 0;
    while (at < source.size())
    {
        Lexer lexer(source.substr(at), ExpressionLanguageVersion::ValV1);
        auto resume = source.size();
        for (auto token = lexer.next(); token.kind != TokenKind::End; token = lexer.next())
        {
            const auto offset = at + token.range.offset;
            if (token.kind == TokenKind::Number && token.text.front() == '.')
            {
                result.append(source.substr(copied, offset - copied));
                result += '0';
                copied = offset;
            }
            else if (token.kind == TokenKind::InvalidCharacter && token.text == "'")
            {
                // A string's periods stay as they are.
                const auto close = source.find('\'', offset + 1);
                resume = close == std::string_view::npos ? source.size() : close + 1;
                break;
            }
        }
        at = resume;
    }
    result.append(source.substr(copied));
    return result;
}

std::string withBpmRemoveUnitsInHertz(std::string_view source)
{
    std::string result;
    std::size_t copied = 0;
    std::size_t at = 0;
    while (at < source.size())
    {
        Lexer lexer(source.substr(at), ExpressionLanguageVersion::ValV1);
        std::vector<Token> tokens;
        auto resume = source.size();
        for (auto token = lexer.next(); token.kind != TokenKind::End; token = lexer.next())
        {
            if (token.kind == TokenKind::InvalidCharacter && token.text == "'")
            {
                // A string's words stay as they are.
                const auto close = source.find('\'', at + token.range.offset + 1);
                resume = close == std::string_view::npos ? source.size() : close + 1;
                break;
            }
            tokens.push_back(token);
        }
        for (std::size_t index = 0; index < tokens.size(); ++index)
        {
            if (tokens[index].kind != TokenKind::Identifier || tokens[index].text != "bpm")
                continue;
            auto next = index + 1;
            while (next < tokens.size() && tokens[next].kind == TokenKind::RightParen)
                ++next;
            if (next == tokens.size() || tokens[next].kind != TokenKind::Identifier
                || tokens[next].text != "removeUnits")
                continue;
            const auto end = at + tokens[index].range.offset + tokens[index].range.length;
            result.append(source.substr(copied, end - copied));
            result += " hz";
            copied = end;
        }
        at = resume;
    }
    result.append(source.substr(copied));
    return result;
}

std::optional<std::string> withSampleFileDurations(
    std::string_view source,
    const std::function<std::optional<double>(std::string_view)>& seconds)
{
    constexpr std::string_view message = "sampleFileDuration";
    if (source.find(message) == std::string_view::npos)
        return std::nullopt;
    std::string result;
    auto changed = false;
    std::size_t at = 0;
    while (at < source.size())
    {
        const auto open = source.find('\'', at);
        const auto close = open == std::string_view::npos ? open : source.find('\'', open + 1);
        if (close == std::string_view::npos)
            break;
        auto after = close + 1;
        while (after < source.size() && std::isspace(static_cast<unsigned char>(source[after])) != 0)
            ++after;
        const auto end = after + message.size();
        const auto sent = source.compare(after, message.size(), message) == 0
            && (end == source.size()
                || (std::isalnum(static_cast<unsigned char>(source[end])) == 0 && source[end] != '_'
                    && source[end] != ':'));
        const auto duration = sent ? seconds(source.substr(open + 1, close - open - 1)) : std::nullopt;
        std::array<char, 512> digits{};
        const auto written = duration.has_value() && std::isfinite(*duration) && *duration > 0.0
            ? std::to_chars(digits.data(), digits.data() + digits.size(), *duration, std::chars_format::fixed)
            : std::to_chars_result{digits.data(), std::errc::invalid_argument};
        if (written.ec != std::errc{})
        {
            result.append(source.substr(at, close + 1 - at));
            at = close + 1;
            continue;
        }
        result.append(source.substr(at, open - at));
        result += "(" + std::string(digits.data(), written.ptr) + ")";
        at = end;
        changed = true;
    }
    if (!changed)
        return std::nullopt;
    result.append(source.substr(at));
    return result;
}

std::string withBpmTimesNumbersRemoveUnitsInHertz(std::string_view source)
{
    std::vector<std::size_t> ends;
    Parser parser(source, ExpressionLanguageVersion::ValV1);
    parser.recordBpmFactors(&ends);
    static_cast<void>(parser.parse());
    std::ranges::sort(ends, std::greater{});
    const auto [first, last] = std::ranges::unique(ends);
    ends.erase(first, last);
    std::string result{source};
    for (const auto end : ends)
        result.insert(end, " hz");
    return result;
}

std::string withMetronomesWithoutVoiceStealing(std::string_view source)
{
    std::vector<SourceRange> ranges;
    Parser parser(source, ExpressionLanguageVersion::ValV1);
    parser.recordMetronomes(&ranges);
    static_cast<void>(parser.parse());
    // Each message in parentheses, then abs: its first five milliseconds
    // back at 1.
    std::vector<std::pair<std::size_t, std::string_view>> insertions;
    for (const auto& range : ranges)
    {
        if (range.offset + range.length > source.size())
            continue;
        insertions.emplace_back(range.offset + range.length, ") abs");
        insertions.emplace_back(range.offset, "(");
    }
    std::ranges::stable_sort(insertions, std::greater{}, &std::pair<std::size_t, std::string_view>::first);
    std::string result{source};
    for (const auto& [at, text] : insertions)
        result.insert(at, text);
    return result;
}

std::optional<std::string> withSignSelectionsStartingAtZero(std::string_view source)
{
    std::vector<SourceRange> ranges;
    Parser parser(source, ExpressionLanguageVersion::ValV1);
    parser.recordSignSelections(&ranges);
    static_cast<void>(parser.parse());
    std::vector<std::size_t> ends;
    for (const auto& range : ranges)
    {
        if (range.offset + range.length <= source.size())
            ends.push_back(range.offset + range.length);
    }
    if (ends.empty())
        return std::nullopt;
    std::ranges::sort(ends, std::greater{});
    std::string result{source};
    for (const auto at : ends)
        result.insert(at, " initially: 0");
    return result;
}

// Whether a source starts with the mark among the marks the compile gives.
[[nodiscard]] static bool markedWith(std::string_view source, std::string_view marker) noexcept
{
    for (auto marks = 0; marks < 2; ++marks)
    {
        if (source.starts_with(marker))
            return true;
        for (const auto other : {timeRunsOnMarker, wholeSoundClocksMarker})
            if (source.starts_with(other))
                source.remove_prefix(other.size());
    }
    return false;
}

std::optional<std::string> withTimeRunningOnInSkippedBranches(
    std::string_view source, const ExpressionDefinitionLibrary& definitions)
{
    if (markedWith(source, timeRunsOnMarker))
        return std::nullopt;
    const auto parsed = parse(source, ExpressionLanguageVersion::ValV1, definitions);
    if (!parsed.expression.has_value() || !parsed.expression->isStateful()
        || !parsed.expression->mayLeaveBranchesUntaken())
        return std::nullopt;
    std::string marked{timeRunsOnMarker};
    marked += source;
    return marked;
}

std::optional<std::string> withWholeSoundClocks(std::string_view source)
{
    if (markedWith(source, wholeSoundClocksMarker) || source.size() > ExpressionLimits::maximumSourceBytes)
        return std::nullopt;
    Lexer lexer(source, ExpressionLanguageVersion::ValV1);
    for (auto token = lexer.next(); token.kind != TokenKind::End; token = lexer.next())
    {
        if (token.kind == TokenKind::Event && (token.text == "RealTime" || token.text == "Time"))
        {
            std::string marked{wholeSoundClocksMarker};
            marked += source;
            return marked;
        }
    }
    return std::nullopt;
}

std::string withPitchDApart(std::string_view source)
{
    std::string result;
    std::size_t copied = 0;
    std::size_t at = 0;
    while (at < source.size())
    {
        Lexer lexer(source.substr(at), ExpressionLanguageVersion::ValV1);
        auto resume = source.size();
        for (auto token = lexer.next(); token.kind != TokenKind::End; token = lexer.next())
        {
            const auto offset = at + token.range.offset;
            if (token.kind == TokenKind::Number && token.text.back() == 'd')
            {
                const auto d = offset + token.range.length - 1;
                result.append(source.substr(copied, d - copied));
                result += ' ';
                copied = d;
            }
            else if (token.kind == TokenKind::InvalidCharacter && token.text == "'")
            {
                // A string's words stay as they are.
                const auto close = source.find('\'', offset + 1);
                resume = close == std::string_view::npos ? source.size() : close + 1;
                break;
            }
        }
        at = resume;
    }
    result.append(source.substr(copied));
    return result;
}

ParseResult parse(
    std::string_view source,
    ExpressionLanguageVersion languageVersion,
    const ExpressionDefinitionLibrary& definitions)
{
    ParseResult result;
    Diagnostic definitionFailure;
    if (!validateDefinitionLibrary(
            languageVersion,
            definitions,
            definitionFailure))
    {
        result.diagnostics.push_back(std::move(definitionFailure));
        return result;
    }
    if (!validateDefinitionPrograms(
            languageVersion,
            definitions,
            definitionFailure))
    {
        result.diagnostics.push_back(std::move(definitionFailure));
        return result;
    }
    // A source withTimeRunningOnInSkippedBranches or withWholeSoundClocks
    // marked, without its marks.
    auto wholeSoundClocks = false;
    auto timeRunsOn = false;
    while (languageVersion == ExpressionLanguageVersion::ValV1)
    {
        if (!wholeSoundClocks && source.starts_with(wholeSoundClocksMarker))
        {
            wholeSoundClocks = true;
            source.remove_prefix(wholeSoundClocksMarker.size());
        }
        else if (!timeRunsOn && source.starts_with(timeRunsOnMarker))
        {
            timeRunsOn = true;
            source.remove_prefix(timeRunsOnMarker.size());
        }
        else
            break;
    }
    // A field holding a program parses as the one expression it gives.
    std::string program;
    if (usesValMessages(languageVersion))
    {
        auto inlined = inlineFieldProgram(source);
        if (!inlined.error.empty())
        {
            result.diagnostics.push_back(makeDiagnostic(
                Diagnostic::Code::UnexpectedToken, {0, source.size()}, std::move(inlined.error)));
            return result;
        }
        if (inlined.expression.has_value())
        {
            program = std::move(*inlined.expression);
            source = program;
        }
    }
    std::string collected;
    if (languageVersion == ExpressionLanguageVersion::ValV1)
    {
        if (auto arrays = withCollectedArrays(source))
        {
            collected = std::move(*arrays);
            source = collected;
        }
    }
    Parser parser(source, languageVersion, &definitions);
    parser.readWholeSoundClocks(wholeSoundClocks);
    auto parsed = parser.parse();
    result.diagnostics = std::move(parsed.diagnostics);
    if (!parsed.storage.has_value())
        return result;

    if (isCompiledExpressionLanguage(languageVersion))
    {
        Diagnostic compileFailure;
        if (!Compiler(*parsed.storage, &definitions).compile(compileFailure))
        {
            result.diagnostics.push_back(std::move(compileFailure));
            return result;
        }
    }
    parsed.storage->timeRunsOnInSkippedBranches = timeRunsOn;
    result.expression = Expression(
        std::make_shared<detail::ExpressionStorage>(
            std::move(*parsed.storage)));
    return result;
}

EvaluationResult evaluate(
    const Expression& expression,
    const EvaluationContext& context,
    EvaluationOptions options)
{
    EvaluationResult result;
    if (expression.storage_ == nullptr)
    {
        result.diagnostics.push_back(makeDiagnostic(
            Diagnostic::Code::ExpectedExpression,
            {0, 0},
            "Expression has no compiled program"));
        return result;
    }

    if (expression.storage_->languageVersion
        == ExpressionLanguageVersion::LegacyV1)
    {
        return LegacyEvaluator(
                   *expression.storage_, context, true)
            .evaluate(options);
    }
    if (expression.storage_->instructions.empty())
    {
        result.diagnostics.push_back(makeDiagnostic(
            Diagnostic::Code::ExpectedExpression,
            {0, 0},
            "Expression has no compiled program"));
        return result;
    }

    const auto execution =
        Executor(*expression.storage_, context).execute(options);
    if (execution.ok)
        result.value = execution.value;
    else
    {
        auto diagnostic = makeDiagnostic(
            execution.failure.code,
            execution.failure.range,
            diagnosticMessage(*expression.storage_, execution.failure));
        if (execution.failure.instructionIndex
            < expression.storage_->instructions.size())
        {
            diagnostic.definitionIndex =
                expression.storage_->instructions[
                    execution.failure.instructionIndex]
                    .definitionIndex;
        }
        result.diagnostics.push_back(std::move(diagnostic));
    }
    return result;
}

bool tryEvaluate(
    const Expression& expression,
    const EvaluationContext& context,
    EvaluationOptions options,
    ExpressionValue& result) noexcept
{
    if (expression.storage_ == nullptr)
    {
        return false;
    }
    if (expression.storage_->languageVersion
        == ExpressionLanguageVersion::LegacyV1)
    {
        return LegacyEvaluator(
                   *expression.storage_, context, false)
            .tryEvaluate(options, result);
    }
    if (expression.storage_->instructions.empty())
        return false;
    const auto execution =
        Executor(*expression.storage_, context).execute(options);
    if (!execution.ok)
        return false;
    result = execution.value;
    return true;
}

bool prepareExpressionState(
    const Expression& expression,
    ExpressionState& state) noexcept
{
    if (expression.storage_ == nullptr
        || expression.storage_->stateSlotCount
            > ExpressionLimits::maximumStateSlots)
    {
        return false;
    }
    state = {};
    state.slotCount = expression.storage_->stateSlotCount;
    state.randomSalt = expression.randomSalt_;
    return true;
}

bool prepareExpressionStateScratch(
    const Expression& expression,
    ExpressionStateScratch& scratch) noexcept
{
    return prepareExpressionState(expression, scratch.candidate);
}

EvaluationResult evaluateStateful(
    const Expression& expression,
    const EvaluationContext& context,
    ExpressionState& state,
    EvaluationOptions options)
{
    ExpressionStateScratch scratch;
    return evaluateStateful(
        expression,
        context,
        state,
        scratch,
        options);
}

EvaluationResult evaluateStateful(
    const Expression& expression,
    const EvaluationContext& context,
    ExpressionState& state,
    ExpressionStateScratch& scratch,
    EvaluationOptions options)
{
    EvaluationResult result;
    if (expression.storage_ == nullptr)
    {
        result.diagnostics.push_back(makeDiagnostic(
            Diagnostic::Code::ExpectedExpression,
            {0, 0},
            "Expression has no compiled program"));
        return result;
    }
    if (expression.storage_->languageVersion
        == ExpressionLanguageVersion::LegacyV1)
    {
        return LegacyEvaluator(
                   *expression.storage_, context, true)
            .evaluate(options);
    }
    if (expression.storage_->instructions.empty())
    {
        result.diagnostics.push_back(makeDiagnostic(
            Diagnostic::Code::ExpectedExpression,
            {0, 0},
            "Expression has no compiled program"));
        return result;
    }

    Executor executor(
        *expression.storage_,
        context,
        &state,
        &scratch.candidate);
    const auto execution = executor.execute(options);
    if (execution.ok)
    {
        result.value = execution.value;
        executor.commitState(state);
    }
    else
    {
        auto diagnostic = makeDiagnostic(
            execution.failure.code,
            execution.failure.range,
            diagnosticMessage(*expression.storage_, execution.failure));
        if (execution.failure.instructionIndex
            < expression.storage_->instructions.size())
        {
            diagnostic.definitionIndex =
                expression.storage_->instructions[
                    execution.failure.instructionIndex]
                    .definitionIndex;
        }
        result.diagnostics.push_back(std::move(diagnostic));
    }
    return result;
}

bool tryEvaluateStateful(
    const Expression& expression,
    const EvaluationContext& context,
    ExpressionState& state,
    EvaluationOptions options,
    ExpressionValue& result) noexcept
{
    ExpressionStateScratch scratch;
    return tryEvaluateStateful(
        expression,
        context,
        state,
        scratch,
        options,
        result);
}

bool tryEvaluateStateful(
    const Expression& expression,
    const EvaluationContext& context,
    ExpressionState& state,
    ExpressionStateScratch& scratch,
    EvaluationOptions options,
    ExpressionValue& result) noexcept
{
    if (expression.storage_ == nullptr)
        return false;
    if (expression.storage_->languageVersion
        == ExpressionLanguageVersion::LegacyV1)
    {
        return LegacyEvaluator(
                   *expression.storage_, context, false)
            .tryEvaluate(options, result);
    }
    if (expression.storage_->instructions.empty())
        return false;
    Executor executor(
        *expression.storage_,
        context,
        &state,
        &scratch.candidate);
    const auto execution = executor.execute(options);
    if (!execution.ok)
        return false;
    result = execution.value;
    executor.commitState(state);
    return true;
}
}
